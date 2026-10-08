#pragma once

namespace mutr {

struct Config {
    int port = 6379;
    bool verbose = false;
    // Worker threads. The main thread accepts and does not run commands.
    // 1 still means one worker, so the process has two threads.
    int threads = 1;
    // Power of two. The process rejects any other value.
    int shards = 64;
    // Linux: sched_setaffinity per worker. Elsewhere this is a no-op.
    bool pin = false;
};

// Returns 1 if the socket or event loop cannot be opened, 2 if shards is invalid.
// SIGINT and SIGTERM stop the acceptor, join the workers, and return 0.
int runServer(const Config& config);

}  // namespace mutr
