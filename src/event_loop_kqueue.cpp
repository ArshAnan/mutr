#include "event_loop.h"

#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <unistd.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>

namespace mutr {

EventLoop::EventLoop() : backend_(::kqueue()) {
    if (backend_ < 0) {
        return;
    }
    const int flags = ::fcntl(backend_, F_GETFD, 0);
    if (flags >= 0) {
        ::fcntl(backend_, F_SETFD, flags | FD_CLOEXEC);
    }
}

EventLoop::~EventLoop() {
    if (backend_ >= 0) {
        ::close(backend_);
    }
}

bool EventLoop::add(int fd, bool readable, bool writable) {
    return update(fd, readable, writable);
}

bool EventLoop::remove(int fd) {
    return update(fd, false, false);
}

bool EventLoop::update(int fd, bool readable, bool writable) {
    if (backend_ < 0) {
        return false;
    }
    Interest current{};
    const auto it = interest_.find(fd);
    if (it != interest_.end()) {
        current = it->second;
    }

    // No EV_CLEAR: filters stay reported while the condition is true.
    std::vector<struct kevent> changes;
    auto push = [&](short filter, bool want, bool have) {
        if (want == have) {
            return;
        }
        struct kevent ev {};
        EV_SET(&ev, static_cast<uintptr_t>(fd), filter, want ? (EV_ADD | EV_ENABLE) : EV_DELETE, 0, 0,
               nullptr);
        changes.push_back(ev);
    };
    push(EVFILT_READ, readable, current.readable);
    push(EVFILT_WRITE, writable, current.writable);
    if (!changes.empty()) {
        const int rc = ::kevent(backend_, changes.data(), static_cast<int>(changes.size()), nullptr, 0, nullptr);
        if (rc < 0) {
            return false;
        }
    }
    if (!readable && !writable) {
        interest_.erase(fd);
    } else {
        interest_[fd] = Interest{readable, writable};
    }
    return true;
}

std::vector<Fired> EventLoop::wait(int timeout_ms) {
    std::vector<Fired> out;
    if (backend_ < 0) {
        return out;
    }
    struct timespec ts {};
    struct timespec* timeout = nullptr;
    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = static_cast<long>(timeout_ms % 1000) * 1000000L;
        timeout = &ts;
    }
    struct kevent evs[64];
    const int n = ::kevent(backend_, nullptr, 0, evs, 64, timeout);
    if (n <= 0) {
        return out;
    }

    // READ and WRITE arrive as separate kevents. Fold them so each fd is
    // handled once; a close in this batch cannot be confused with a second hit.
    std::unordered_map<int, Fired> by_fd;
    for (int i = 0; i < n; ++i) {
        const int fd = static_cast<int>(evs[i].ident);
        Fired& fired = by_fd[fd];
        fired.fd = fd;
        if (evs[i].filter == EVFILT_READ) {
            fired.readable = true;
        }
        if (evs[i].filter == EVFILT_WRITE) {
            fired.writable = true;
        }
        if ((evs[i].flags & EV_EOF) != 0 || (evs[i].flags & EV_ERROR) != 0) {
            fired.hangup = true;
        }
    }
    out.reserve(by_fd.size());
    for (const auto& item : by_fd) {
        out.push_back(item.second);
    }
    return out;
}

}  // namespace mutr
