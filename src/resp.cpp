#include "resp.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <system_error>

namespace mutr {
namespace {

enum class WalkKind { Ok, NeedMore, Bad };

struct Len {
    WalkKind kind;
    std::int64_t value;
    std::size_t next;
};

struct Scan {
    ParseStatus status;
    std::size_t consumed;
    const char* error;
};

enum class Mode { Scan, Build };

// Integer terminated by CRLF. Rejects more than 20 digits so a client cannot
// force us to scan an unbounded run before we know it cannot fit in int64.
Len parseCrLfInt(std::string_view s, std::size_t i) {
    if (i >= s.size()) {
        return {WalkKind::NeedMore, 0, i};
    }
    const std::size_t start = i;
    if (s[i] == '-') {
        ++i;
        if (i >= s.size()) {
            return {WalkKind::NeedMore, 0, i};
        }
    }
    if (static_cast<unsigned char>(s[i]) < '0' || static_cast<unsigned char>(s[i]) > '9') {
        return {WalkKind::Bad, 0, i};
    }
    std::size_t digits = 0;
    while (i < s.size() && static_cast<unsigned char>(s[i]) >= '0' &&
           static_cast<unsigned char>(s[i]) <= '9') {
        ++i;
        ++digits;
        if (digits > 20) {
            return {WalkKind::Bad, 0, i};
        }
    }
    if (i >= s.size()) {
        return {WalkKind::NeedMore, 0, i};
    }
    if (s[i] != '\r') {
        return {WalkKind::Bad, 0, i};
    }
    if (i + 1 >= s.size()) {
        return {WalkKind::NeedMore, 0, i};
    }
    if (s[i + 1] != '\n') {
        return {WalkKind::Bad, 0, i};
    }

    std::int64_t value = 0;
    const auto res = std::from_chars(s.data() + start, s.data() + i, value);
    if (res.ec != std::errc() || res.ptr != s.data() + i) {
        return {WalkKind::Bad, 0, i};
    }
    return {WalkKind::Ok, value, i + 2};
}

Scan fail(const char* error) {
    return {ParseStatus::Error, 0, error};
}

Scan need() {
    return {ParseStatus::NeedMoreData, 0, nullptr};
}

Scan parseInline(std::string_view s, const Limits& lim, Mode mode, Command* cmd) {
    const std::size_t window = std::min(s.size(), lim.max_inline + 2);
    for (std::size_t i = 0; i < window; ++i) {
        if (s[i] != '\n') {
            continue;
        }
        if (i == 0 || s[i - 1] != '\r') {
            return fail("ERR Protocol error: expected CRLF");
        }
        const std::size_t line_len = i - 1;
        if (line_len > lim.max_inline) {
            return fail("ERR Protocol error: inline command too long");
        }
        bool any = false;
        for (std::size_t k = 0; k < line_len; ++k) {
            if (s[k] != ' ') {
                any = true;
                break;
            }
        }
        if (!any) {
            return fail("ERR Protocol error: empty inline command");
        }
        if (mode == Mode::Build) {
            std::size_t a = 0;
            while (a < line_len) {
                while (a < line_len && s[a] == ' ') {
                    ++a;
                }
                if (a >= line_len) {
                    break;
                }
                std::size_t b = a;
                while (b < line_len && s[b] != ' ') {
                    ++b;
                }
                std::string tok(s.data() + a, b - a);
                if (cmd->name.empty()) {
                    cmd->name = std::move(tok);
                } else {
                    cmd->args.push_back(std::move(tok));
                }
                a = b;
            }
        }
        return {ParseStatus::Ok, i + 1, nullptr};
    }
    if (s.size() > lim.max_inline) {
        return fail("ERR Protocol error: inline command too long");
    }
    return need();
}

Scan parseArray(std::string_view s, const Limits& lim, Mode mode, Command* cmd) {
    const Len len = parseCrLfInt(s, 1);
    if (len.kind == WalkKind::NeedMore) {
        return need();
    }
    if (len.kind == WalkKind::Bad) {
        return fail("ERR Protocol error: invalid length");
    }
    if (len.value < 1) {
        return fail("ERR Protocol error: invalid array length");
    }
    if (static_cast<std::uint64_t>(len.value) > lim.max_array) {
        return fail("ERR Protocol error: array too large");
    }

    std::size_t i = len.next;
    for (std::int64_t el = 0; el < len.value; ++el) {
        if (i >= s.size()) {
            return need();
        }
        if (s[i] != '$') {
            return fail("ERR Protocol error: expected '$'");
        }
        const Len bulk = parseCrLfInt(s, i + 1);
        if (bulk.kind == WalkKind::NeedMore) {
            return need();
        }
        if (bulk.kind == WalkKind::Bad) {
            return fail("ERR Protocol error: invalid length");
        }
        if (bulk.value < 0) {
            return fail("ERR Protocol error: invalid bulk length");
        }
        if (static_cast<std::uint64_t>(bulk.value) > lim.max_bulk) {
            return fail("ERR Protocol error: bulk string too large");
        }
        if (bulk.next > s.size()) {
            return need();
        }
        const std::size_t data_at = bulk.next;
        const std::size_t nbytes = static_cast<std::size_t>(bulk.value);
        if (nbytes > std::numeric_limits<std::size_t>::max() - 2) {
            return fail("ERR Protocol error: bulk string too large");
        }
        if (s.size() - data_at < nbytes + 2) {
            return need();
        }
        if (s[data_at + nbytes] != '\r' || s[data_at + nbytes + 1] != '\n') {
            return fail("ERR Protocol error: expected CRLF");
        }
        if (mode == Mode::Build) {
            // Length constructor: bytes may contain interior NUL.
            std::string field(s.data() + data_at, nbytes);
            if (el == 0) {
                cmd->name = std::move(field);
            } else {
                cmd->args.push_back(std::move(field));
            }
        }
        i = data_at + nbytes + 2;
    }
    return {ParseStatus::Ok, i, nullptr};
}

Scan scanCommand(std::string_view data, const Limits& limits, Mode mode, Command* cmd) {
    if (data.empty()) {
        return need();
    }
    if (data[0] == '*') {
        return parseArray(data, limits, mode, cmd);
    }
    return parseInline(data, limits, mode, cmd);
}

}  // namespace

ParseResult parseOne(std::string_view data, const Limits& limits) {
    ParseResult result;
    const Scan scanned = scanCommand(data, limits, Mode::Scan, nullptr);
    if (scanned.status != ParseStatus::Ok) {
        result.status = scanned.status;
        if (scanned.error != nullptr) {
            result.error = scanned.error;
        }
        return result;
    }

    Command cmd;
    const Scan built = scanCommand(data, limits, Mode::Build, &cmd);
    if (built.status != ParseStatus::Ok) {
        result.status = built.status;
        if (built.error != nullptr) {
            result.error = built.error;
        }
        return result;
    }
    result.status = ParseStatus::Ok;
    result.consumed = built.consumed;
    result.command = std::move(cmd);
    return result;
}

std::string respSimple(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 3);
    out.push_back('+');
    out.append(text.data(), text.size());
    out.append("\r\n");
    return out;
}

std::string respError(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 3);
    out.push_back('-');
    out.append(text.data(), text.size());
    out.append("\r\n");
    return out;
}

std::string respBulk(std::string_view data) {
    const std::string len = std::to_string(data.size());
    std::string out;
    out.reserve(data.size() + len.size() + 5);
    out.push_back('$');
    out.append(len);
    out.append("\r\n");
    out.append(data.data(), data.size());
    out.append("\r\n");
    return out;
}

std::string respNullBulk() {
    return "$-1\r\n";
}

std::string respInteger(std::int64_t value) {
    return ":" + std::to_string(value) + "\r\n";
}

}  // namespace mutr
