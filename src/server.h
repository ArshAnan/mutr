#pragma once

namespace mutr {

struct Config {
    int port = 6379;
    bool verbose = false;
    // Parsed and stored. Values other than 1 are rejected: there is still one thread.
    int threads = 1;
};

// One thread, level-triggered event loop. Returns 1 if the socket cannot be
// opened, 2 if threads is not 1.
int runServer(const Config& config);

}  // namespace mutr
