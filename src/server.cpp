#ifdef __linux__
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif

#include "server.h"

#include "event_loop.h"
#include "net.h"
#include "session.h"
#include "store.h"

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef __linux__
#include <sched.h>
#endif

namespace mutr {
namespace {

constexpr int kBacklog = 128;

// The signal handler writes one byte here. Lock-free so the handler may load it.
std::atomic<int> g_shutdown_write{-1};

void handleShutdownSignal(int) {
    const int fd = g_shutdown_write.load(std::memory_order_relaxed);
    if (fd < 0) {
        return;
    }
    const char byte = 1;
    // The handler cannot report a short write. A full pipe still wakes the acceptor.
    if (::write(fd, &byte, 1) < 0) {
        return;
    }
}

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

// One worker's counters. alignas pads the object out to a cache line so the
// next field in the worker cannot share that line. Only that worker writes it.
struct alignas(64) WorkerStats {
    std::uint64_t commands = 0;
    std::uint64_t bytes_in = 0;
    std::uint64_t bytes_out = 0;
};
static_assert(alignof(WorkerStats) == 64, "stats alignment");
static_assert(sizeof(WorkerStats) % 64 == 0, "stats span whole cache lines");

std::mutex g_log_mu;

void logLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_log_mu);
    std::cerr << line << '\n';
}

bool pinCurrentThread(int index) {
#ifdef __linux__
    long cpus = ::sysconf(_SC_NPROCESSORS_ONLN);
    if (cpus < 1) {
        cpus = 1;
    }
    const int cpu = index % static_cast<int>(cpus);
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return ::sched_setaffinity(0, sizeof(set), &set) == 0;
#else
    (void)index;
    return true;
#endif
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

bool makePipe(int ends[2]) {
    if (::pipe(ends) != 0) {
        return false;
    }
    if (!setNonBlocking(ends[0]) || !setNonBlocking(ends[1])) {
        ::close(ends[0]);
        ::close(ends[1]);
        ends[0] = -1;
        ends[1] = -1;
        return false;
    }
    return true;
}

class Worker {
public:
    Worker(Store& store, const Config& config, int index) : store_(store), config_(config), index_(index) {
        int ends[2] = {-1, -1};
        if (!makePipe(ends)) {
            return;
        }
        wake_r_ = ends[0];
        wake_w_ = ends[1];
    }

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    ~Worker() {
        if (thread_.joinable()) {
            requestStop();
            thread_.join();
        }
        if (wake_r_ >= 0) {
            ::close(wake_r_);
        }
        if (wake_w_ >= 0) {
            ::close(wake_w_);
        }
    }

    bool ok() const { return wake_r_ >= 0 && loop_.ok(); }

    void start() { thread_ = std::thread([this] { run(); }); }

    bool waitReady() {
        std::unique_lock<std::mutex> lock(mu_);
        ready_cv_.wait(lock, [this] { return ready_; });
        return ready_ok_;
    }

    bool pinFailed() const { return pin_failed_; }

    void enqueue(int fd) { poke(fd, false); }

    void requestStop() { poke(-1, true); }

    void join() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    // Safe only after join: the worker is the only writer, and join
    // happens-before this read.
    WorkerStats stats() const { return stats_; }

private:
    void poke(int fd, bool stop) {
        bool need_write = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (fd >= 0) {
                inbox_.push_back(fd);
            }
            if (stop) {
                stop_ = true;
            }
            need_write = !signaled_;
            signaled_ = true;
        }
        if (need_write) {
            writeWake();
        }
    }

    void writeWake() {
        const char byte = 1;
        for (;;) {
            const ssize_t n = ::write(wake_w_, &byte, 1);
            if (n == 1 || (n < 0 && errno != EINTR)) {
                return;
            }
        }
    }

    void run() {
        if (config_.pin && !pinCurrentThread(index_)) {
            pin_failed_ = true;
        }
        const bool loop_ok = loop_.ok() && wake_r_ >= 0 && loop_.add(wake_r_, true, false);
        {
            std::lock_guard<std::mutex> lock(mu_);
            ready_ok_ = loop_ok;
            ready_ = true;
        }
        ready_cv_.notify_one();
        if (!loop_ok) {
            return;
        }

        bool stopping = false;
        while (!stopping) {
            const std::vector<Fired> events = loop_.wait(-1);
            for (const Fired& ev : events) {
                if (ev.fd == wake_r_) {
                    if (onWake()) {
                        stopping = true;
                    }
                    continue;
                }
                if (conns_.find(ev.fd) == conns_.end()) {
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

    // Returns true when the worker should exit.
    bool onWake() {
        char buf[64];
        for (;;) {
            const ssize_t n = ::read(wake_r_, buf, sizeof(buf));
            if (n > 0) {
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            break;
        }

        std::deque<int> fds;
        bool stop = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            fds.swap(inbox_);
            stop = stop_;
            signaled_ = false;
        }
        for (int fd : fds) {
            adopt(fd);
        }
        if (!stop) {
            return false;
        }
        // Flush what the socket will take without blocking the process on a
        // client that never reads. An idle connection would otherwise stall
        // shutdown forever.
        std::vector<int> open;
        open.reserve(conns_.size());
        for (const auto& entry : conns_) {
            open.push_back(entry.first);
        }
        for (int fd : open) {
            const auto it = conns_.find(fd);
            if (it == conns_.end()) {
                continue;
            }
            it->second.close_when_flushed = true;
            flush(it->second);
            if (conns_.find(fd) != conns_.end()) {
                closeConn(fd);
            }
        }
        return true;
    }

    void adopt(int fd) {
        if (!loop_.add(fd, true, false)) {
            ::close(fd);
            return;
        }
        Conn conn;
        conn.fd = fd;
        conns_.emplace(fd, std::move(conn));
        if (config_.verbose) {
            logLine("accepted " + std::to_string(fd));
        }
    }

    void closeConn(int fd) {
        loop_.remove(fd);
        ::close(fd);
        if (config_.verbose) {
            logLine("closed " + std::to_string(fd));
        }
        conns_.erase(fd);
    }

    bool flush(Conn& conn) {
        while (conn.out_off < conn.out.size()) {
            const WriteOutcome wrote = writeSome(
                conn.fd, conn.out.data() + conn.out_off, conn.out.size() - conn.out_off);
            if (wrote.kind == WriteKind::Ok) {
                conn.out_off += wrote.n;
                stats_.bytes_out += wrote.n;
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
        loop_.update(conn.fd, true, false);
        return true;
    }

    bool process(Conn& conn) {
        for (;;) {
            compactOut(conn);
            const std::size_t in_before = conn.in.size();
            std::uint64_t commands = 0;
            if (!handleInput(store_, conn.in, conn.out, {}, &commands)) {
                conn.close_when_flushed = true;
            }
            stats_.commands += commands;
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
                stats_.bytes_in += static_cast<std::uint64_t>(n);
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

    Store& store_;
    Config config_;
    int index_ = 0;
    EventLoop loop_;
    std::unordered_map<int, Conn> conns_;
    std::mutex mu_;
    std::condition_variable ready_cv_;
    std::deque<int> inbox_;
    bool signaled_ = false;
    bool stop_ = false;
    bool ready_ = false;
    bool ready_ok_ = false;
    bool pin_failed_ = false;
    int wake_r_ = -1;
    int wake_w_ = -1;
    WorkerStats stats_;
    std::thread thread_;
};

class Server {
public:
    explicit Server(const Config& config)
        : config_(config), store_(static_cast<std::size_t>(config.shards)) {}

    int run() {
        struct sigaction pipe_action {};
        pipe_action.sa_handler = SIG_IGN;
        if (::sigaction(SIGPIPE, &pipe_action, nullptr) != 0) {
            std::perror("sigaction");
            return 1;
        }

        sigset_t blocked;
        sigemptyset(&blocked);
        sigaddset(&blocked, SIGINT);
        sigaddset(&blocked, SIGTERM);
        // Workers inherit this mask, so shutdown signals are delivered to the
        // acceptor (this thread) after it unblocks below.
        if (::pthread_sigmask(SIG_BLOCK, &blocked, nullptr) != 0) {
            std::perror("pthread_sigmask");
            return 1;
        }

        int shutdown_pipe[2] = {-1, -1};
        if (!makePipe(shutdown_pipe)) {
            std::perror("pipe");
            return 1;
        }
        g_shutdown_write.store(shutdown_pipe[1], std::memory_order_relaxed);

        struct sigaction stop_action {};
        stop_action.sa_handler = handleShutdownSignal;
        sigemptyset(&stop_action.sa_mask);
        stop_action.sa_flags = 0;
        if (::sigaction(SIGINT, &stop_action, nullptr) != 0 ||
            ::sigaction(SIGTERM, &stop_action, nullptr) != 0) {
            std::perror("sigaction");
            ::close(shutdown_pipe[0]);
            ::close(shutdown_pipe[1]);
            return 1;
        }

        workers_.reserve(static_cast<std::size_t>(config_.threads));
        for (int i = 0; i < config_.threads; ++i) {
            workers_.push_back(std::make_unique<Worker>(store_, config_, i));
            if (!workers_.back()->ok()) {
                std::perror("worker");
                stopJoin();
                ::close(shutdown_pipe[0]);
                ::close(shutdown_pipe[1]);
                return 1;
            }
            workers_.back()->start();
        }
        for (const auto& worker : workers_) {
            if (!worker->waitReady()) {
                std::perror("worker");
                stopJoin();
                ::close(shutdown_pipe[0]);
                ::close(shutdown_pipe[1]);
                return 1;
            }
        }

        if (::pthread_sigmask(SIG_UNBLOCK, &blocked, nullptr) != 0) {
            std::perror("pthread_sigmask");
            stopJoin();
            ::close(shutdown_pipe[0]);
            ::close(shutdown_pipe[1]);
            return 1;
        }

        std::uint16_t bound = 0;
        listen_fd_ = listenTcp(static_cast<std::uint16_t>(config_.port), &bound);
        if (listen_fd_ < 0 || !setNonBlocking(listen_fd_) || !loop_.ok() ||
            !loop_.add(listen_fd_, true, false) || !loop_.add(shutdown_pipe[0], true, false)) {
            std::perror("listen");
            if (listen_fd_ >= 0) {
                ::close(listen_fd_);
                listen_fd_ = -1;
            }
            stopJoin();
            ::close(shutdown_pipe[0]);
            ::close(shutdown_pipe[1]);
            return 1;
        }

        // Tests parse this exact first line. Pin warnings come after it.
        std::cerr << "listening " << bound << "\n" << std::flush;
        for (std::size_t i = 0; i < workers_.size(); ++i) {
            if (workers_[i]->pinFailed()) {
                logLine("pin failed for worker " + std::to_string(i));
            }
        }

        for (;;) {
            const std::vector<Fired> events = loop_.wait(-1);
            bool stopping = false;
            for (const Fired& ev : events) {
                if (ev.fd == shutdown_pipe[0]) {
                    stopping = true;
                    continue;
                }
                if (ev.fd == listen_fd_ && ev.readable && !stopping) {
                    acceptAll();
                }
            }
            if (stopping) {
                break;
            }
        }

        if (listen_fd_ >= 0) {
            loop_.remove(listen_fd_);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        // Join must not be interrupted by a second SIGINT.
        ::pthread_sigmask(SIG_BLOCK, &blocked, nullptr);
        stopJoin();
        if (config_.verbose) {
            WorkerStats total;
            for (const auto& worker : workers_) {
                const WorkerStats stats = worker->stats();
                total.commands += stats.commands;
                total.bytes_in += stats.bytes_in;
                total.bytes_out += stats.bytes_out;
            }
            logLine("stats commands=" + std::to_string(total.commands) +
                    " bytes_in=" + std::to_string(total.bytes_in) +
                    " bytes_out=" + std::to_string(total.bytes_out));
        }
        ::close(shutdown_pipe[0]);
        ::close(shutdown_pipe[1]);
        g_shutdown_write.store(-1, std::memory_order_relaxed);
        return 0;
    }

private:
    void stopJoin() {
        for (const auto& worker : workers_) {
            worker->requestStop();
        }
        for (const auto& worker : workers_) {
            worker->join();
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
                if (errno == EMFILE || errno == ENFILE) {
                    ::poll(nullptr, 0, 10);
                    return;
                }
                if (config_.verbose) {
                    std::perror("accept");
                }
                return;
            }
            if (!setNonBlocking(fd) || !setNoSigPipe(fd)) {
                ::close(fd);
                continue;
            }
            // One connection stays on the worker that receives it.
            // The listen socket is not SO_REUSEPORT; this thread is the only acceptor.
            workers_[static_cast<std::size_t>(next_)]->enqueue(fd);
            next_ = (next_ + 1) % config_.threads;
        }
    }

    Config config_;
    Store store_;
    EventLoop loop_;
    int listen_fd_ = -1;
    int next_ = 0;
    std::vector<std::unique_ptr<Worker>> workers_;
};

}  // namespace

int runServer(const Config& config) {
    const int shards = config.shards;
    if (shards < 1 || static_cast<unsigned>(shards) > (1u << 20) || (shards & (shards - 1)) != 0) {
        std::cerr << "invalid shards\n";
        return 2;
    }
    if (config.threads < 1) {
        std::cerr << "invalid threads\n";
        return 2;
    }
    Server server(config);
    return server.run();
}

}  // namespace mutr
