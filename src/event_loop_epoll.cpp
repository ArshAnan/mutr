#include "event_loop.h"

#include <cerrno>
#include <unistd.h>
#include <sys/epoll.h>

namespace mutr {

EventLoop::EventLoop() : backend_(::epoll_create1(EPOLL_CLOEXEC)) {}

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
    const auto it = interest_.find(fd);
    const bool exists = it != interest_.end();
    if (!readable && !writable) {
        if (exists) {
            if (::epoll_ctl(backend_, EPOLL_CTL_DEL, fd, nullptr) < 0 && errno != ENOENT) {
                return false;
            }
            interest_.erase(fd);
        }
        return true;
    }

    // Level-triggered: EPOLLET is not set. EPOLLOUT stays hot while the
    // socket can accept writes, so the caller must drop write interest
    // once its output buffer is empty.
    epoll_event ev {};
    ev.data.fd = fd;
    ev.events = 0;
    if (readable) {
        ev.events |= EPOLLIN;
    }
    if (writable) {
        ev.events |= EPOLLOUT;
    }
    const int op = exists ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    if (::epoll_ctl(backend_, op, fd, &ev) < 0) {
        return false;
    }
    interest_[fd] = Interest{readable, writable};
    return true;
}

std::vector<Fired> EventLoop::wait(int timeout_ms) {
    std::vector<Fired> out;
    if (backend_ < 0) {
        return out;
    }
    epoll_event evs[64];
    const int n = ::epoll_wait(backend_, evs, 64, timeout_ms);
    if (n <= 0) {
        return out;
    }
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        Fired fired;
        fired.fd = evs[i].data.fd;
        fired.readable = (evs[i].events & (EPOLLIN | EPOLLPRI)) != 0;
        fired.writable = (evs[i].events & EPOLLOUT) != 0;
        fired.hangup = (evs[i].events & (EPOLLHUP | EPOLLERR)) != 0;
        out.push_back(fired);
    }
    return out;
}

}  // namespace mutr
