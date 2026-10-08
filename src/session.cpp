#include "session.h"

#include "commands.h"

namespace mutr {

bool handleInput(Store& store, std::string& inbound, std::string& outbound, const Limits& limits) {
    std::size_t off = 0;
    bool keep = true;
    while (off < inbound.size()) {
        // Stop once replies exceed the cap so one read cannot grow output
        // without bound. One reply may still be larger than the cap (a big GET).
        // The caller flushes and calls again; unparsed commands stay in inbound.
        if (outbound.size() > limits.max_buffer) {
            break;
        }
        const ParseResult parsed = parseOne(
            std::string_view(inbound.data() + off, inbound.size() - off), limits);
        if (parsed.status == ParseStatus::NeedMoreData) {
            if (inbound.size() - off > limits.max_buffer) {
                outbound += respError("ERR Protocol error: request too large");
                keep = false;
            }
            break;
        }
        if (parsed.status == ParseStatus::Error) {
            const std::string& msg = parsed.error.empty() ? "ERR Protocol error" : parsed.error;
            outbound += respError(msg);
            keep = false;
            break;
        }
        if (parsed.consumed == 0 || off + parsed.consumed > inbound.size()) {
            outbound += respError("ERR Protocol error: invalid parser state");
            keep = false;
            break;
        }
        if (parsed.consumed > limits.max_buffer) {
            outbound += respError("ERR Protocol error: request too large");
            keep = false;
            break;
        }
        off += parsed.consumed;
        outbound += execute(store, parsed.command);
    }
    if (off > 0) {
        inbound.erase(0, off);
    }
    return keep;
}

}  // namespace mutr
