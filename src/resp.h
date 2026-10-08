#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mutr {

constexpr std::size_t kMaxBulkLen = std::size_t{64} * 1024 * 1024;
constexpr std::size_t kMaxArrayLen = 1000000;
constexpr std::size_t kMaxInlineLen = std::size_t{64} * 1024;
// One command has to fit. A max-sized bulk plus a modest key and framing does.
constexpr std::size_t kMaxBufferLen = kMaxBulkLen + (std::size_t{1} << 20);

struct Limits {
    std::size_t max_bulk = kMaxBulkLen;
    std::size_t max_array = kMaxArrayLen;
    std::size_t max_inline = kMaxInlineLen;
    std::size_t max_buffer = kMaxBufferLen;
};

enum class ParseStatus {
    NeedMoreData,
    Ok,
    Error,
};

struct Command {
    std::string name;
    std::vector<std::string> args;
};

struct ParseResult {
    ParseStatus status = ParseStatus::NeedMoreData;
    std::size_t consumed = 0;
    Command command;
    std::string error;
};

// Parse exactly one command at the front of data.
// NeedMoreData: consumed is 0 and the caller keeps every byte.
// Ok: consumed is the byte length of that command; bytes after it are leftovers.
// Error: the connection should be sent `error` and closed. Nothing is consumed.
[[nodiscard]] ParseResult parseOne(std::string_view data, const Limits& limits = {});

std::string respSimple(std::string_view text);
std::string respError(std::string_view text);
std::string respBulk(std::string_view data);
std::string respNullBulk();
std::string respInteger(std::int64_t value);

}  // namespace mutr
