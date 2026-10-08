#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mutr {

// Single-threaded map. There is no lock: callers must not share a Store
// across threads. Expired keys stay allocated until the next access.
class Store {
public:
    struct IncrResult {
        bool ok = false;
        std::int64_t value = 0;
    };

    // Test hook. Production never calls this; now() is steady_clock::now().
    void setNowForTest(std::chrono::steady_clock::time_point t);

    // ttl nullopt keeps the key with no expiry. ttl of 0 deletes it.
    // Returns false if a positive ttl does not fit in the clock range;
    // the previous value is left unchanged in that case.
    bool set(std::string key, std::string value, std::optional<std::chrono::milliseconds> ttl);

    std::optional<std::string> get(const std::string& key);
    int del(const std::vector<std::string>& keys);
    int exists(const std::vector<std::string>& keys);
    IncrResult incr(const std::string& key);
    void mset(const std::vector<std::string>& key_values);
    std::vector<std::optional<std::string>> mget(const std::vector<std::string>& keys);

private:
    struct Value {
        std::string data;
        bool has_expiry = false;
        std::chrono::steady_clock::time_point expiry{};
    };

    bool expired(const Value& value) const;
    std::chrono::steady_clock::time_point now() const;

    std::unordered_map<std::string, Value> map_;
    bool use_virtual_ = false;
    std::chrono::steady_clock::time_point virtual_now_{};
};

}  // namespace mutr
