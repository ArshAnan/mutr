#pragma once

#include <unordered_map>
#include <vector>

namespace mutr {

struct Fired {
    int fd = -1;
    bool readable = false;
    bool writable = false;
    bool hangup = false;
};

// Level-triggered readiness. The kqueue backend does not set EV_CLEAR.
// The epoll backend does not set EPOLLET. Write interest has to be removed
// when the output buffer is empty, or a level-triggered loop spins.
class EventLoop {
public:
    EventLoop();
    ~EventLoop();
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    bool ok() const { return backend_ >= 0; }
    bool add(int fd, bool readable, bool writable);
    bool update(int fd, bool readable, bool writable);
    bool remove(int fd);

    // timeout_ms < 0 blocks. 0 polls. EINTR yields an empty list.
    std::vector<Fired> wait(int timeout_ms);

private:
    struct Interest {
        bool readable = false;
        bool writable = false;
    };

    int backend_ = -1;
    std::unordered_map<int, Interest> interest_;
};

}  // namespace mutr
