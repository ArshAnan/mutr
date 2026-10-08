#pragma once

namespace mutr {

struct Config {
    int port = 6379;
    bool verbose = false;
};

// Blocks serving clients one at a time. Returns 1 if the socket cannot be opened.
int runServer(const Config& config);

}  // namespace mutr
