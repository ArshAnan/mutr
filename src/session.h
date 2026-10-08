#pragma once

#include "resp.h"
#include "store.h"

#include <string>

namespace mutr {

// Parse every complete command in inbound, append replies to outbound, and
// erase consumed bytes. A partial command stays in inbound.
// Returns false on a protocol or limit error. One RESP error is appended and
// the caller must write outbound and close the connection.
// Command errors (unknown command, bad arity) are normal replies and return true.
[[nodiscard]] bool handleInput(Store& store,
                               std::string& inbound,
                               std::string& outbound,
                               const Limits& limits = {});

}  // namespace mutr
