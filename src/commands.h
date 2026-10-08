#pragma once

#include "resp.h"
#include "store.h"

#include <string>

namespace mutr {

// Returns one complete RESP reply. Command errors do not close the connection.
// Only parseOne / handleInput protocol errors do.
std::string execute(Store& store, const Command& cmd);

}  // namespace mutr
