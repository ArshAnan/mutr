#include "store.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace mutr {
namespace {

std::optional<std::int64_t> parseInt(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    // from_chars accepts '-' but not '+'. Redis integers allow one leading '+'.
    if (text.front() == '+') {
        text.remove_prefix(1);
        if (text.empty()) {
            return std::nullopt;
        }
    }
    std::int64_t value = 0;
    const auto res = std::from_chars(text.data(), text.data() + text.size(), value);
    if (res.ec != std::errc() || res.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

}  // namespace

Store::Store(std::size_t shard_count) : shard_count_(shard_count) {
    if (shard_count == 0 || shard_count > (std::size_t{1} << 20) ||
        (shard_count & (shard_count - 1)) != 0) {
        throw std::invalid_argument("shard count must be a power of two");
    }
    while ((std::size_t{1} << shard_bits_) < shard_count_) {
        ++shard_bits_;
    }
    shards_.reset(new Shard[shard_count_]);
}

std::uint64_t Store::hashKey(std::string_view key) {
    // FNV-1a, then a splitmix64 finalizer. FNV's high bits are weak on short
    // keys; the shard index is taken from those bits.
    std::uint64_t h = 14695981039346656037ull;
    for (unsigned char c : key) {
        h ^= c;
        h *= 1099511628211ull;
    }
    h = (h ^ (h >> 30)) * 0xbf58476d1ce4e5b9ull;
    h = (h ^ (h >> 27)) * 0x94d049bb133111ebull;
    h = h ^ (h >> 31);
    return h;
}

Store::Located Store::locate(std::string_view key) const {
    Located loc;
    // One shard: every key maps to index 0. A 64-bit shift is undefined,
    // so this case cannot be written as hash >> (64 - log2(N)).
    if (shard_bits_ == 0) {
        return loc;
    }
    const std::uint64_t h = hashKey(key);
    loc.shard = static_cast<std::size_t>(h >> (64 - shard_bits_));
    return loc;
}

std::size_t Store::shardIndex(std::string_view key) const {
    return locate(key).shard;
}

std::vector<std::unique_lock<std::mutex>> Store::lockShards(const std::vector<std::size_t>& shards) {
    std::vector<std::size_t> order = shards;
    std::sort(order.begin(), order.end());
    order.erase(std::unique(order.begin(), order.end()), order.end());
    std::vector<std::unique_lock<std::mutex>> locks;
    locks.reserve(order.size());
    for (std::size_t index : order) {
        locks.emplace_back(shards_[index].mu);
    }
    return locks;
}

void Store::setNowForTest(std::chrono::steady_clock::time_point t) {
    use_virtual_ = true;
    virtual_now_ = t;
}

std::chrono::steady_clock::time_point Store::now() const {
    if (use_virtual_) {
        return virtual_now_;
    }
    return std::chrono::steady_clock::now();
}

bool Store::expired(const Value& value) const {
    return value.has_expiry && now() >= value.expiry;
}

bool Store::setUnlocked(Shard& shard, std::string key, std::string value,
                        std::optional<std::chrono::milliseconds> ttl) {
    if (ttl && ttl->count() < 0) {
        return false;
    }
    if (ttl && ttl->count() == 0) {
        shard.map.erase(key);
        return true;
    }

    Value stored;
    stored.data = std::move(value);
    if (ttl) {
        const auto now_tp = now();
        const auto room = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::time_point::max() - now_tp);
        if (*ttl > room) {
            return false;
        }
        stored.has_expiry = true;
        stored.expiry = now_tp + *ttl;
    }
    shard.map[std::move(key)] = std::move(stored);
    return true;
}

std::optional<std::string> Store::getUnlocked(Shard& shard, const std::string& key) {
    const auto it = shard.map.find(key);
    if (it == shard.map.end()) {
        return std::nullopt;
    }
    if (expired(it->second)) {
        shard.map.erase(it);
        return std::nullopt;
    }
    return it->second.data;
}

bool Store::delUnlocked(Shard& shard, const std::string& key) {
    const auto it = shard.map.find(key);
    if (it == shard.map.end()) {
        return false;
    }
    if (expired(it->second)) {
        shard.map.erase(it);
        return false;
    }
    shard.map.erase(it);
    return true;
}

bool Store::existsUnlocked(Shard& shard, const std::string& key) {
    const auto it = shard.map.find(key);
    if (it == shard.map.end()) {
        return false;
    }
    if (expired(it->second)) {
        shard.map.erase(it);
        return false;
    }
    return true;
}

Store::IncrResult Store::incrUnlocked(Shard& shard, const std::string& key) {
    const auto it = shard.map.find(key);
    std::int64_t current = 0;
    bool keep_expiry = false;
    std::chrono::steady_clock::time_point expiry{};
    if (it != shard.map.end()) {
        if (expired(it->second)) {
            shard.map.erase(it);
        } else {
            const auto parsed = parseInt(it->second.data);
            if (!parsed) {
                return {};
            }
            if (*parsed == std::numeric_limits<std::int64_t>::max()) {
                return {};
            }
            current = *parsed;
            keep_expiry = it->second.has_expiry;
            expiry = it->second.expiry;
        }
    }

    Value stored;
    stored.data = std::to_string(current + 1);
    stored.has_expiry = keep_expiry;
    stored.expiry = expiry;
    shard.map[key] = std::move(stored);
    return {true, current + 1};
}

bool Store::set(std::string key, std::string value, std::optional<std::chrono::milliseconds> ttl) {
    const Located loc = locate(key);
    const auto locks = lockShards({loc.shard});
    return setUnlocked(shards_[loc.shard], std::move(key), std::move(value), ttl);
}

std::optional<std::string> Store::get(const std::string& key) {
    const Located loc = locate(key);
    const auto locks = lockShards({loc.shard});
    return getUnlocked(shards_[loc.shard], key);
}

int Store::del(const std::vector<std::string>& keys) {
    std::vector<std::size_t> shards(keys.size());
    for (std::size_t i = 0; i < keys.size(); ++i) {
        shards[i] = locate(keys[i]).shard;
    }
    const auto locks = lockShards(shards);
    int removed = 0;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (delUnlocked(shards_[shards[i]], keys[i])) {
            ++removed;
        }
    }
    return removed;
}

int Store::exists(const std::vector<std::string>& keys) {
    std::vector<std::size_t> shards(keys.size());
    for (std::size_t i = 0; i < keys.size(); ++i) {
        shards[i] = locate(keys[i]).shard;
    }
    const auto locks = lockShards(shards);
    int count = 0;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (existsUnlocked(shards_[shards[i]], keys[i])) {
            ++count;
        }
    }
    return count;
}

Store::IncrResult Store::incr(const std::string& key) {
    const Located loc = locate(key);
    const auto locks = lockShards({loc.shard});
    return incrUnlocked(shards_[loc.shard], key);
}

void Store::mset(const std::vector<std::string>& key_values) {
    const std::size_t pairs = key_values.size() / 2;
    std::vector<std::size_t> shards(pairs);
    for (std::size_t i = 0; i < pairs; ++i) {
        shards[i] = locate(key_values[i * 2]).shard;
    }
    const auto locks = lockShards(shards);
    for (std::size_t i = 0; i < pairs; ++i) {
        setUnlocked(shards_[shards[i]], key_values[i * 2], key_values[i * 2 + 1], std::nullopt);
    }
}

std::vector<std::optional<std::string>> Store::mget(const std::vector<std::string>& keys) {
    std::vector<std::size_t> shards(keys.size());
    for (std::size_t i = 0; i < keys.size(); ++i) {
        shards[i] = locate(keys[i]).shard;
    }
    const auto locks = lockShards(shards);
    std::vector<std::optional<std::string>> out;
    out.reserve(keys.size());
    for (std::size_t i = 0; i < keys.size(); ++i) {
        out.push_back(getUnlocked(shards_[shards[i]], keys[i]));
    }
    return out;
}

}  // namespace mutr
