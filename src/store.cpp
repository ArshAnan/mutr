#include "store.h"

#include <charconv>
#include <limits>
#include <system_error>

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

bool Store::set(std::string key, std::string value, std::optional<std::chrono::milliseconds> ttl) {
    if (ttl && ttl->count() < 0) {
        return false;
    }
    if (ttl && ttl->count() == 0) {
        map_.erase(key);
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
    map_[std::move(key)] = std::move(stored);
    return true;
}

std::optional<std::string> Store::get(const std::string& key) {
    const auto it = map_.find(key);
    if (it == map_.end()) {
        return std::nullopt;
    }
    if (expired(it->second)) {
        map_.erase(it);
        return std::nullopt;
    }
    return it->second.data;
}

int Store::del(const std::vector<std::string>& keys) {
    int removed = 0;
    for (const auto& key : keys) {
        const auto it = map_.find(key);
        if (it == map_.end()) {
            continue;
        }
        if (expired(it->second)) {
            map_.erase(it);
            continue;
        }
        map_.erase(it);
        ++removed;
    }
    return removed;
}

int Store::exists(const std::vector<std::string>& keys) {
    int count = 0;
    for (const auto& key : keys) {
        const auto it = map_.find(key);
        if (it == map_.end()) {
            continue;
        }
        if (expired(it->second)) {
            map_.erase(it);
            continue;
        }
        ++count;
    }
    return count;
}

Store::IncrResult Store::incr(const std::string& key) {
    const auto it = map_.find(key);
    std::int64_t current = 0;
    bool keep_expiry = false;
    std::chrono::steady_clock::time_point expiry{};
    if (it != map_.end()) {
        if (expired(it->second)) {
            map_.erase(it);
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
    map_[key] = std::move(stored);
    return {true, current + 1};
}

void Store::mset(const std::vector<std::string>& key_values) {
    for (std::size_t i = 0; i + 1 < key_values.size(); i += 2) {
        set(key_values[i], key_values[i + 1], std::nullopt);
    }
}

std::vector<std::optional<std::string>> Store::mget(const std::vector<std::string>& keys) {
    std::vector<std::optional<std::string>> out;
    out.reserve(keys.size());
    for (const auto& key : keys) {
        out.push_back(get(key));
    }
    return out;
}

}  // namespace mutr
