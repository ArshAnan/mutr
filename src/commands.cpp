#include "commands.h"

#include <charconv>
#include <limits>
#include <system_error>

namespace mutr {
namespace {

bool sameCmd(std::string_view got, std::string_view upper) {
    if (got.size() != upper.size()) {
        return false;
    }
    for (std::size_t i = 0; i < got.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(got[i]);
        if (c >= 'a' && c <= 'z') {
            c = static_cast<unsigned char>(c - 'a' + 'A');
        }
        if (c != static_cast<unsigned char>(upper[i])) {
            return false;
        }
    }
    return true;
}

std::string arity(const char* lower_name) {
    std::string msg = "ERR wrong number of arguments for '";
    msg += lower_name;
    msg += "' command";
    return respError(msg);
}

std::string unknown(std::string_view name) {
    bool echo = !name.empty() && name.size() <= 64;
    for (char raw : name) {
        const unsigned char c = static_cast<unsigned char>(raw);
        if (c <= 32 || c > 126) {
            echo = false;
            break;
        }
    }
    if (!echo) {
        return respError("ERR unknown command");
    }
    std::string msg = "ERR unknown command '";
    msg.append(name.data(), name.size());
    msg += "'";
    return respError(msg);
}

std::optional<std::int64_t> parseWhole(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    // from_chars accepts '-' but not '+'. One leading '+' is a valid Redis integer.
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

std::string cmdPing(const Command& cmd) {
    if (cmd.args.empty()) {
        return respSimple("PONG");
    }
    if (cmd.args.size() == 1) {
        return respBulk(cmd.args[0]);
    }
    return arity("ping");
}

std::string cmdSet(Store& store, const Command& cmd) {
    if (cmd.args.size() != 2 && cmd.args.size() != 4) {
        return arity("set");
    }
    std::optional<std::chrono::milliseconds> ttl;
    if (cmd.args.size() == 4) {
        const bool ex = sameCmd(cmd.args[2], "EX");
        const bool px = sameCmd(cmd.args[2], "PX");
        if (!ex && !px) {
            return respError("ERR syntax error");
        }
        const auto n = parseWhole(cmd.args[3]);
        if (!n || *n < 0) {
            return respError("ERR invalid expire time in 'set' command");
        }
        if (ex) {
            if (*n > std::numeric_limits<std::int64_t>::max() / 1000) {
                return respError("ERR invalid expire time in 'set' command");
            }
            ttl = std::chrono::milliseconds(*n * 1000);
        } else {
            ttl = std::chrono::milliseconds(*n);
        }
    }
    if (!store.set(cmd.args[0], cmd.args[1], ttl)) {
        return respError("ERR invalid expire time in 'set' command");
    }
    return respSimple("OK");
}

std::string cmdGet(Store& store, const Command& cmd) {
    if (cmd.args.size() != 1) {
        return arity("get");
    }
    const auto value = store.get(cmd.args[0]);
    if (!value) {
        return respNullBulk();
    }
    return respBulk(*value);
}

std::string cmdDel(Store& store, const Command& cmd) {
    if (cmd.args.empty()) {
        return arity("del");
    }
    return respInteger(store.del(cmd.args));
}

std::string cmdExists(Store& store, const Command& cmd) {
    if (cmd.args.empty()) {
        return arity("exists");
    }
    return respInteger(store.exists(cmd.args));
}

std::string cmdIncr(Store& store, const Command& cmd) {
    if (cmd.args.size() != 1) {
        return arity("incr");
    }
    const auto result = store.incr(cmd.args[0]);
    if (!result.ok) {
        return respError("ERR value is not an integer or out of range");
    }
    return respInteger(result.value);
}

std::string cmdMset(Store& store, const Command& cmd) {
    if (cmd.args.size() < 2 || cmd.args.size() % 2 != 0) {
        return arity("mset");
    }
    store.mset(cmd.args);
    return respSimple("OK");
}

std::string cmdMget(Store& store, const Command& cmd) {
    if (cmd.args.empty()) {
        return arity("mget");
    }
    const auto values = store.mget(cmd.args);
    std::string out = "*" + std::to_string(values.size()) + "\r\n";
    for (const auto& value : values) {
        if (!value) {
            out += respNullBulk();
        } else {
            out += respBulk(*value);
        }
    }
    return out;
}

}  // namespace

std::string execute(Store& store, const Command& cmd) {
    if (sameCmd(cmd.name, "PING")) {
        return cmdPing(cmd);
    }
    if (sameCmd(cmd.name, "SET")) {
        return cmdSet(store, cmd);
    }
    if (sameCmd(cmd.name, "GET")) {
        return cmdGet(store, cmd);
    }
    if (sameCmd(cmd.name, "DEL")) {
        return cmdDel(store, cmd);
    }
    if (sameCmd(cmd.name, "EXISTS")) {
        return cmdExists(store, cmd);
    }
    if (sameCmd(cmd.name, "INCR")) {
        return cmdIncr(store, cmd);
    }
    if (sameCmd(cmd.name, "MSET")) {
        return cmdMset(store, cmd);
    }
    if (sameCmd(cmd.name, "MGET")) {
        return cmdMget(store, cmd);
    }
    return unknown(cmd.name);
}

}  // namespace mutr
