#include "server.h"

#include "event_loop.h"
#include "net.h"
#include "session.h"
#include "store.h"

#include <cerrno>
#include <cstdint>
#include <iostream>
#include <signal.h>
#include <unordered_map>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace mutr {
namespace {

constexpr int kBacklog = 128;

struct Conn {
    int fd = -1;
    std::string in;
    std::string out;
    std::size_t out_off = 0;
    bool close_when_flushed = false;
};

std::size_t pendingOut(const Conn& conn) {
    return conn.out.size() - conn.out_off;
}

void compactOut(Conn& conn) {
    if (conn.out_off == 0) {
        return;
    }
    conn.out.erase(0, conn.out_off);
    conn.out_off = 0;
}

int listenTcp(std::uint16_t port, std::uint16_t* bound) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    int on = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
        ::close(fd);
        return -1;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    if (::listen(fd, kBacklog) < 0) {
        ::close(fd);
        return -1;
    }
    sockaddr_in got{};
    socklen_t got_len = sizeof(got);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&got), &got_len) < 0) {
        ::close(fd);
        return -1;
    }
    *bound = ntohs(got.sin_port);
    return fd;
}

class Server {
public:
    explicit Server(const Config& config)
        : config_(config), store_(static_cast<std::size_t>(config.shards)) {}

    int run() {
        if (config_.threads != 1) {
            std::cerr << "only --threads 1 is implemented\n";
            return 2;
        }
        struct sigaction sa {};
        sa.sa_handler = SIG_IGN;
        if (::sigaction(SIGPIPE, &sa, nullptr) != 0) {
            std::perror("sigaction");
            return 1;
        }
        if (!loop_.ok()) {
            std::perror("event loop");
            return 1;
        }

        std::uint16_t bound = 0;
        listen_fd_ = listenTcp(static_cast<std::uint16_t>(config_.port), &bound);
        if (listen_fd_ < 0) {
            std::perror("listen");
            return 1;
        }
        if (!setNonBlocking(listen_fd_) || !loop_.add(listen_fd_, true, false)) {
            ::close(listen_fd_);
            std::perror("listen");
            return 1;
        }

        std::cerr << "listening " << bound << "\n" << std::flush;

        for (;;) {
            const std::vector<Fired> events = loop_.wait(-1);
            for (const Fired& ev : events) {
                if (ev.fd == listen_fd_) {
                    if (ev.readable) {
                        acceptAll();
                    }
                    continue;
                }
                if (ev.readable || ev.hangup) {
                    onRead(ev.fd);
                }
                if (conns_.find(ev.fd) != conns_.end() && ev.writable) {
                    onWrite(ev.fd);
                }
            }
        }
    }

private:
    void closeConn(int fd) {
        loop_.remove(fd);
        ::close(fd);
        if (config_.verbose) {
            std::cerr << "closed " << fd << "\n";
        }
        conns_.erase(fd);
    }

    // Returns false if the connection was closed.
    bool flush(Conn& conn) {
        while (conn.out_off < conn.out.size()) {
            const WriteOutcome wrote = writeSome(
                conn.fd, conn.out.data() + conn.out_off, conn.out.size() - conn.out_off);
            if (wrote.kind == WriteKind::Ok) {
                conn.out_off += wrote.n;
                continue;
            }
            if (wrote.kind == WriteKind::Blocked) {
                compactOut(conn);
                const bool want_read = !conn.close_when_flushed && pendingOut(conn) <= kMaxBufferLen;
                loop_.update(conn.fd, want_read, true);
                return true;
            }
            closeConn(conn.fd);
            return false;
        }
        conn.out.clear();
        conn.out_off = 0;
        if (conn.close_when_flushed) {
            closeConn(conn.fd);
            return false;
        }
        // Output is empty: drop write interest or the level-triggered loop spins.
        loop_.update(conn.fd, true, false);
        return true;
    }

    bool process(Conn& conn) {
        for (;;) {
            compactOut(conn);
            const std::size_t in_before = conn.in.size();
            if (!handleInput(store_, conn.in, conn.out)) {
                conn.close_when_flushed = true;
            }
            if (!flush(conn)) {
                return false;
            }
            if (conn.out_off < conn.out.size()) {
                return true;
            }
            if (conn.close_when_flushed) {
                closeConn(conn.fd);
                return false;
            }
            if (conn.in.size() == in_before) {
                return true;
            }
        }
    }

    void onRead(int fd) {
        char buf[16 * 1024];
        bool eof = false;
        bool hard_error = false;
        for (;;) {
            const auto it = conns_.find(fd);
            if (it == conns_.end()) {
                return;
            }
            if (it->second.close_when_flushed || pendingOut(it->second) > kMaxBufferLen) {
                break;
            }
            const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n > 0) {
                it->second.in.append(buf, static_cast<std::size_t>(n));
                if (!process(it->second)) {
                    return;
                }
                const auto again = conns_.find(fd);
                if (again == conns_.end()) {
                    return;
                }
                if (pendingOut(again->second) > 0) {
                    return;
                }
                continue;
            }
            if (n == 0) {
                eof = true;
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            hard_error = true;
            break;
        }

        const auto it = conns_.find(fd);
        if (it == conns_.end()) {
            return;
        }
        if (hard_error || eof) {
            it->second.close_when_flushed = true;
            flush(it->second);
        }
    }

    void onWrite(int fd) {
        const auto it = conns_.find(fd);
        if (it == conns_.end()) {
            return;
        }
        if (!flush(it->second)) {
            return;
        }
        const auto again = conns_.find(fd);
        if (again == conns_.end()) {
            return;
        }
        if (!again->second.in.empty() && pendingOut(again->second) == 0) {
            process(again->second);
        }
    }

    void acceptAll() {
        for (;;) {
            sockaddr_in addr{};
            socklen_t len = sizeof(addr);
            const int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
            if (fd < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return;
                }
                // The listen socket stays readable. A tight loop here would
                // peg a core when the process is out of fds.
                if (errno == EMFILE || errno == ENFILE) {
                    ::poll(nullptr, 0, 10);
                    return;
                }
                if (config_.verbose) {
                    std::perror("accept");
                }
                return;
            }
            if (!setNonBlocking(fd) || !setNoSigPipe(fd) || !loop_.add(fd, true, false)) {
                ::close(fd);
                continue;
            }
            Conn conn;
            conn.fd = fd;
            conns_.emplace(fd, std::move(conn));
            if (config_.verbose) {
                std::cerr << "accepted " << fd << "\n";
            }
        }
    }

    Config config_;
    EventLoop loop_;
    Store store_;
    int listen_fd_ = -1;
    std::unordered_map<int, Conn> conns_;
};

}  // namespace

int runServer(const Config& config) {
    const int shards = config.shards;
    if (shards < 1 || static_cast<unsigned>(shards) > (1u << 20) || (shards & (shards - 1)) != 0) {
        std::cerr << "invalid shards\n";
        return 2;
    }
    Server server(config);
    return server.run();
}

}  // namespace mutr
