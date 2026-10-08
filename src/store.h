#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mutr {

// Per-shard mutexes. Not lock-free. A Store may be shared across threads.
// Expired keys stay allocated until the next access, and that access drops
// them under the same shard lock as the read or update.
class Store {
public:
    static constexpr std::size_t kDefaultShards = 64;

    struct IncrResult {
        bool ok = false;
        std::int64_t value = 0;
    };

    // shard_count must be a power of two in [1, 1 << 20].
    explicit Store(std::size_t shard_count = kDefaultShards);

    std::size_t shardCount() const { return shard_count_; }

    // 64-bit hash used only to pick a shard. std::unordered_map hashes the
    // key again for its own buckets; see docs/DESIGN.md.
    static std::uint64_t hashKey(std::string_view key);
    std::size_t shardIndex(std::string_view key) const;

    // Test hook. Not safe to call concurrently with other Store methods.
    void setNowForTest(std::chrono::steady_clock::time_point t);

    // ttl nullopt keeps the key with no expiry. ttl of 0 deletes it.
    // Returns false if a positive ttl does not fit in the clock range;
    // the previous value is left unchanged in that case.
    bool set(std::string key, std::string value, std::optional<std::chrono::milliseconds> ttl);

    std::optional<std::string> get(const std::string& key);
    int del(const std::vector<std::string>& keys);
    int exists(const std::vector<std::string>& keys);
    IncrResult incr(const std::string& key);

    // MSET, MGET, DEL, and EXISTS lock every shard they touch, in ascending
    // shard-index order, and hold those locks until the whole command finishes.
    // A concurrent command sees none of an MSET or all of it.
    void mset(const std::vector<std::string>& key_values);
    std::vector<std::optional<std::string>> mget(const std::vector<std::string>& keys);

private:
    struct Value {
        std::string data;
        bool has_expiry = false;
        std::chrono::steady_clock::time_point expiry{};
    };

    // alignas(64) makes sizeof a multiple of 64, so adjacent shards in the
    // array start on different cache lines and their mutexes do not share one.
    struct alignas(64) Shard {
        std::mutex mu;
        std::unordered_map<std::string, Value> map;
    };
    static_assert(alignof(Shard) == 64, "shard alignment");
    static_assert(sizeof(Shard) % 64 == 0, "shard spans whole cache lines");

    struct Located {
        std::size_t shard = 0;
    };

    Located locate(std::string_view key) const;
    std::vector<std::unique_lock<std::mutex>> lockShards(const std::vector<std::size_t>& shards);
    bool expired(const Value& value) const;
    std::chrono::steady_clock::time_point now() const;

    bool setUnlocked(Shard& shard, std::string key, std::string value,
                     std::optional<std::chrono::milliseconds> ttl);
    std::optional<std::string> getUnlocked(Shard& shard, const std::string& key);
    bool delUnlocked(Shard& shard, const std::string& key);
    bool existsUnlocked(Shard& shard, const std::string& key);
    IncrResult incrUnlocked(Shard& shard, const std::string& key);

    std::size_t shard_count_ = 0;
    unsigned shard_bits_ = 0;
    std::unique_ptr<Shard[]> shards_;
    bool use_virtual_ = false;
    std::chrono::steady_clock::time_point virtual_now_{};
};

}  // namespace mutr
