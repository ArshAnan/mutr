#include "net.h"

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>

namespace mutr {
namespace {

int sendFlags() {
#ifdef MSG_NOSIGNAL
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

}  // namespace

bool setNoSigPipe(int fd) {
#ifdef SO_NOSIGPIPE
    int on = 1;
    return ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)) == 0;
#else
    (void)fd;
    return true;
#endif
}

bool setNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

WriteOutcome writeSome(int fd, const char* data, std::size_t len) {
    if (len == 0) {
        return {WriteKind::Ok, 0};
    }
    for (;;) {
        const ssize_t n = ::send(fd, data, len, sendFlags());
        if (n > 0) {
            return {WriteKind::Ok, static_cast<std::size_t>(n)};
        }
        if (n == 0) {
            return {WriteKind::Error, 0};
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return {WriteKind::Blocked, 0};
        }
        return {WriteKind::Error, 0};
    }
}

bool writeAll(int fd, const char* data, std::size_t len) {
    while (len > 0) {
        const WriteOutcome wrote = writeSome(fd, data, len);
        if (wrote.kind == WriteKind::Ok) {
            if (wrote.n == 0 || wrote.n > len) {
                return false;
            }
            data += wrote.n;
            len -= wrote.n;
            continue;
        }
        if (wrote.kind == WriteKind::Blocked) {
            pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLOUT;
            int rc = 0;
            do {
                rc = ::poll(&pfd, 1, -1);
            } while (rc < 0 && errno == EINTR);
            if (rc <= 0) {
                return false;
            }
            continue;
        }
        return false;
    }
    return true;
}

}  // namespace mutr
