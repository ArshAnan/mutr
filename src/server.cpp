#include "server.h"

#include "net.h"
#include "session.h"
#include "store.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <signal.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace mutr {
namespace {

constexpr int kBacklog = 128;

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

// Serves a single blocking connection until it closes or a protocol error.
void serveClient(int fd, Store& store) {
    std::string inbound;
    std::string outbound;
    char buf[16 * 1024];
    for (;;) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        if (n == 0) {
            return;
        }
        inbound.append(buf, static_cast<std::size_t>(n));
        const bool keep = handleInput(store, inbound, outbound);
        if (!outbound.empty()) {
            if (!writeAll(fd, outbound.data(), outbound.size())) {
                return;
            }
            outbound.clear();
        }
        if (!keep) {
            return;
        }
    }
}

}  // namespace

int runServer(const Config& config) {
    struct sigaction sa {};
    sa.sa_handler = SIG_IGN;
    if (::sigaction(SIGPIPE, &sa, nullptr) != 0) {
        std::perror("sigaction");
        return 1;
    }

    std::uint16_t bound = 0;
    const int listen_fd = listenTcp(static_cast<std::uint16_t>(config.port), &bound);
    if (listen_fd < 0) {
        std::perror("listen");
        return 1;
    }

    // One line on stderr so tests can learn a port-0 binding. Not a per-command log.
    std::cerr << "listening " << bound << "\n" << std::flush;

    Store store;
    for (;;) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        const int client = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::perror("accept");
            continue;
        }
        if (!setNoSigPipe(client)) {
            ::close(client);
            continue;
        }
        if (config.verbose) {
            std::cerr << "accepted " << client << "\n";
        }
        serveClient(client, store);
        ::close(client);
        if (config.verbose) {
            std::cerr << "closed " << client << "\n";
        }
    }
}

}  // namespace mutr
