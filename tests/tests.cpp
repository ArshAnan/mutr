#include "commands.h"
#include "event_loop.h"
#include "net.h"
#include "resp.h"
#include "session.h"
#include "store.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

int g_failed = 0;
std::string g_bin;

// fork duplicates the C++ stream buffers. TSan flushes them in the child,
// which reprints every line still sitting in the parent buffer.
void flushStdio() {
    std::cout.flush();
    std::cerr.flush();
}

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << " CHECK failed: " << #cond \
                      << "\n";                                                      \
            ++g_failed;                                                             \
        }                                                                           \
    } while (0)

std::string respCommand(const std::vector<std::string>& parts) {
    std::string out = "*" + std::to_string(parts.size()) + "\r\n";
    for (const auto& part : parts) {
        out += "$" + std::to_string(part.size()) + "\r\n";
        out.append(part);
        out += "\r\n";
    }
    return out;
}

mutr::Command makeCmd(std::string name, std::vector<std::string> args = {}) {
    mutr::Command cmd;
    cmd.name = std::move(name);
    cmd.args = std::move(args);
    return cmd;
}

struct Expect {
    std::string name;
    std::vector<std::string> args;
};

void driveSplits(const std::string& blob,
                 const std::vector<Expect>& expect,
                 bool one_byte,
                 std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<std::size_t> dist(1, 31);
    std::string pending;
    std::vector<mutr::Command> got;
    std::size_t i = 0;
    while (i < blob.size()) {
        std::size_t n = one_byte ? 1 : dist(rng);
        if (n > blob.size() - i) {
            n = blob.size() - i;
        }
        pending.append(blob.data() + i, n);
        i += n;
        std::size_t off = 0;
        while (off < pending.size()) {
            const auto parsed = mutr::parseOne(
                std::string_view(pending.data() + off, pending.size() - off));
            if (parsed.status == mutr::ParseStatus::NeedMoreData) {
                break;
            }
            if (parsed.status != mutr::ParseStatus::Ok || parsed.consumed == 0 ||
                off + parsed.consumed > pending.size()) {
                std::cerr << "split parse failed seed " << seed << " error [" << parsed.error << "]\n";
                ++g_failed;
                return;
            }
            off += parsed.consumed;
            got.push_back(parsed.command);
        }
        if (off > 0) {
            pending.erase(0, off);
        }
    }
    if (!pending.empty() || got.size() != expect.size()) {
        std::cerr << "split leftover " << pending.size() << " got " << got.size() << " expect "
                  << expect.size() << " seed " << seed << "\n";
        ++g_failed;
        return;
    }
    for (std::size_t c = 0; c < expect.size(); ++c) {
        CHECK(got[c].name == expect[c].name);
        CHECK(got[c].args == expect[c].args);
    }
}

void test_fragmentation_and_pipeline() {
    const std::string ping = respCommand({"PING"});
    const auto one = mutr::parseOne(ping);
    CHECK(one.status == mutr::ParseStatus::Ok);
    CHECK(one.consumed == ping.size());
    CHECK(one.command.name == "PING");
    CHECK(one.command.args.empty());

    const std::string two = ping + ping;
    const auto first = mutr::parseOne(two);
    CHECK(first.status == mutr::ParseStatus::Ok);
    CHECK(first.consumed == ping.size());
    const auto second = mutr::parseOne(std::string_view(two).substr(first.consumed));
    CHECK(second.status == mutr::ParseStatus::Ok);
    CHECK(second.consumed == ping.size());
    CHECK(first.consumed + second.consumed == two.size());

    std::string partial;
    for (char ch : ping) {
        partial.push_back(ch);
        const auto parsed = mutr::parseOne(partial);
        if (partial.size() < ping.size()) {
            CHECK(parsed.status == mutr::ParseStatus::NeedMoreData);
            CHECK(parsed.consumed == 0);
        } else {
            CHECK(parsed.status == mutr::ParseStatus::Ok);
            CHECK(parsed.consumed == ping.size());
        }
    }

    mutr::Store store;
    std::string inbound = "*1\r\n$4\r\nPI";
    std::string outbound;
    CHECK(mutr::handleInput(store, inbound, outbound));
    CHECK(outbound.empty());
    CHECK(inbound == "*1\r\n$4\r\nPI");
    inbound += "NG\r\n*1\r\n$4\r\nPING\r\n";
    CHECK(mutr::handleInput(store, inbound, outbound));
    CHECK(outbound == "+PONG\r\n+PONG\r\n");
    CHECK(inbound.empty());

    std::string batch;
    for (int i = 0; i < 100; ++i) {
        batch += ping;
    }
    std::string batch_out;
    CHECK(mutr::handleInput(store, batch, batch_out));
    CHECK(batch.empty());
    CHECK(batch_out.size() == 100 * std::string("+PONG\r\n").size());

    const std::string value("v\0\r\n*$", 6);
    std::string raw = respCommand({"SET", std::string("k\0k", 3), value});
    raw += respCommand({"GET", std::string("k\0k", 3)});
    std::string bin_out;
    CHECK(mutr::handleInput(store, raw, bin_out));
    CHECK(bin_out == "+OK\r\n" + mutr::respBulk(value));
}

void test_random_and_large_splits() {
    std::string blob;
    std::vector<Expect> expect;
    auto add_resp = [&](std::vector<std::string> parts) {
        blob += respCommand(parts);
        Expect item;
        item.name = parts[0];
        if (parts.size() > 1) {
            item.args.assign(parts.begin() + 1, parts.end());
        }
        expect.push_back(std::move(item));
    };
    auto add_inline = [&](const std::string& line, std::string name, std::vector<std::string> args) {
        blob += line;
        blob += "\r\n";
        expect.push_back(Expect{std::move(name), std::move(args)});
    };

    std::string every_byte;
    every_byte.resize(256);
    for (int b = 0; b < 256; ++b) {
        every_byte[static_cast<std::size_t>(b)] = static_cast<char>(b);
    }
    std::string big(64 * 1024, 'Q');
    big[0] = '\0';
    big[100] = '\r';
    big[101] = '\n';
    big[102] = '*';
    big[103] = '$';

    add_resp({"PING"});
    add_resp({"SET", std::string("a\0b", 3), std::string("*\r\n$", 4)});
    add_inline("ping", "ping", {});
    add_inline("PING hello", "PING", {"hello"});
    add_resp({"GET", std::string("a\0b", 3)});
    add_resp({"SET", "wide", every_byte});
    add_resp({"SET", "bulk64k", big});
    add_resp({"INCR", "n"});
    add_resp({"MSET", "k", "v", "k2", "v2"});
    add_resp({"MGET", "k", "missing"});

    driveSplits(blob, expect, true, 1);
    for (std::uint32_t seed : {1u, 2u, 7u, 42u, 99u}) {
        driveSplits(blob, expect, false, seed);
    }

    std::string meg(1024 * 1024, '\0');
    for (std::size_t i = 0; i < meg.size(); ++i) {
        meg[i] = static_cast<char>(i & 0xff);
    }
    std::string mega_blob = respCommand({"SET", "m", meg});
    std::vector<Expect> mega_expect{Expect{"SET", {"m", meg}}};
    driveSplits(mega_blob, mega_expect, false, 1234);
}

void test_malformed_does_not_throw() {
    const char* bad[] = {
        "",
        "*",
        "*\r\n",
        "*0\r\n",
        "*-1\r\n",
        "*1\r\n",
        "*1\r\n$",
        "*1\r\n$-1\r\n",
        "*1\r\n$-2\r\n",
        "*1\r\n$1\r\nXY\n",
        "*1\r\n#1\r\n",
        "PING",
        "PING\r",
        "PING\n",
        "\r\n",
        "   \r\n",
    };
    for (const char* text : bad) {
        try {
            const auto parsed = mutr::parseOne(text);
            CHECK(parsed.status != mutr::ParseStatus::Ok);
            CHECK(parsed.consumed == 0);
        } catch (const std::exception& ex) {
            std::cerr << "threw on [" << text << "]: " << ex.what() << "\n";
            ++g_failed;
        } catch (...) {
            std::cerr << "threw unknown on [" << text << "]\n";
            ++g_failed;
        }
    }

    std::string digits = "*";
    digits.append(21, '9');
    const auto overflow = mutr::parseOne(digits);
    CHECK(overflow.status == mutr::ParseStatus::Error);

    std::string twenty = "*1\r\n$";
    twenty.append(20, '9');
    CHECK(mutr::parseOne(twenty).status == mutr::ParseStatus::NeedMoreData);
    twenty += "\r\n";
    CHECK(mutr::parseOne(twenty).status == mutr::ParseStatus::Error);

    mutr::Limits limits;
    limits.max_bulk = 4;
    limits.max_array = 2;
    const auto too_big = mutr::parseOne("*1\r\n$5\r\nHELLO\r\n", limits);
    CHECK(too_big.status == mutr::ParseStatus::Error);
    CHECK(too_big.error.find("bulk") != std::string::npos);

    const auto too_many = mutr::parseOne("*3\r\n", limits);
    CHECK(too_many.status == mutr::ParseStatus::Error);
    CHECK(too_many.error.find("array") != std::string::npos);

    const auto exactly = mutr::parseOne(respCommand({"PING"}), limits);
    CHECK(exactly.status == mutr::ParseStatus::Ok);

    mutr::Store store;
    std::string inbound = respCommand({"PING"}) + "*x\r\n" + respCommand({"PING"});
    std::string outbound;
    CHECK(!mutr::handleInput(store, inbound, outbound));
    CHECK(outbound == "+PONG\r\n-ERR Protocol error: invalid length\r\n");

    mutr::Limits tiny;
    tiny.max_buffer = 8;
    std::string buffered = "PINGPING";
    std::string buf_out;
    CHECK(mutr::handleInput(store, buffered, buf_out, tiny));
    CHECK(buf_out.empty());
    buffered.push_back('X');
    CHECK(!mutr::handleInput(store, buffered, buf_out, tiny));
    CHECK(buf_out == "-ERR Protocol error: request too large\r\n");
}

void test_commands() {
    mutr::Store store;
    CHECK(mutr::execute(store, makeCmd("PING")) == "+PONG\r\n");
    CHECK(mutr::execute(store, makeCmd("ping")) == "+PONG\r\n");
    CHECK(mutr::execute(store, makeCmd("Ping", {"hi"})) == mutr::respBulk("hi"));
    CHECK(mutr::execute(store, makeCmd("PING", {"", })) == "$0\r\n\r\n");
    CHECK(mutr::execute(store, makeCmd("PING", {"a", "b"})).find("-ERR wrong number") == 0);

    CHECK(mutr::execute(store, makeCmd("SET", {"a", "b"})) == "+OK\r\n");
    CHECK(mutr::execute(store, makeCmd("get", {"a"})) == mutr::respBulk("b"));
    CHECK(mutr::execute(store, makeCmd("GET", {"missing"})) == "$-1\r\n");
    CHECK(mutr::execute(store, makeCmd("SET", {"a"})).find("-ERR wrong number") == 0);
    CHECK(mutr::execute(store, makeCmd("GET", {"a"})) == mutr::respBulk("b"));

    const std::string blob("x\0y", 3);
    CHECK(mutr::execute(store, makeCmd("SET", {"bin", blob})) == "+OK\r\n");
    CHECK(mutr::execute(store, makeCmd("GET", {"bin"})) == mutr::respBulk(blob));

    CHECK(mutr::execute(store, makeCmd("EXISTS", {"a", "missing", "a"})) == ":2\r\n");
    CHECK(mutr::execute(store, makeCmd("DEL", {"a", "missing", "a"})) == ":1\r\n");
    CHECK(mutr::execute(store, makeCmd("EXISTS", {"a"})) == ":0\r\n");
    CHECK(mutr::execute(store, makeCmd("DEL")).find("-ERR wrong number") == 0);
    CHECK(mutr::execute(store, makeCmd("EXISTS")).find("-ERR wrong number") == 0);

    CHECK(mutr::execute(store, makeCmd("INCR", {"n"})) == ":1\r\n");
    CHECK(mutr::execute(store, makeCmd("INCR", {"n"})) == ":2\r\n");
    CHECK(mutr::execute(store, makeCmd("incr", {"n"})) == ":3\r\n");
    CHECK(mutr::execute(store, makeCmd("GET", {"n"})) == mutr::respBulk("3"));
    CHECK(mutr::execute(store, makeCmd("SET", {"z", "00"})) == "+OK\r\n");
    CHECK(mutr::execute(store, makeCmd("INCR", {"z"})) == ":1\r\n");
    CHECK(mutr::execute(store, makeCmd("SET", {"p", "+10"})) == "+OK\r\n");
    CHECK(mutr::execute(store, makeCmd("INCR", {"p"})) == ":11\r\n");
    CHECK(mutr::execute(store, makeCmd("SET", {"neg", "-5"})) == "+OK\r\n");
    CHECK(mutr::execute(store, makeCmd("INCR", {"neg"})) == ":-4\r\n");
    CHECK(mutr::execute(store, makeCmd("SET", {"bad", "10 "})) == "+OK\r\n");
    CHECK(mutr::execute(store, makeCmd("INCR", {"bad"})).find("-ERR value is not an integer") == 0);
    CHECK(mutr::execute(store, makeCmd("GET", {"bad"})) == mutr::respBulk("10 "));
    CHECK(mutr::execute(store, makeCmd("SET", {"top", "9223372036854775807"})) == "+OK\r\n");
    CHECK(mutr::execute(store, makeCmd("INCR", {"top"})).find("-ERR value is not an integer") == 0);
    CHECK(mutr::execute(store, makeCmd("GET", {"top"})) == mutr::respBulk("9223372036854775807"));
    CHECK(mutr::execute(store, makeCmd("INCR")).find("-ERR wrong number") == 0);

    CHECK(mutr::execute(store, makeCmd("MSET", {"k1", "v1", "k2", "v2"})) == "+OK\r\n");
    CHECK(mutr::execute(store, makeCmd("MGET", {"k1", "missing", "k2"})) ==
          "*3\r\n$2\r\nv1\r\n$-1\r\n$2\r\nv2\r\n");
    CHECK(mutr::execute(store, makeCmd("MSET", {"only"})).find("-ERR wrong number") == 0);
    CHECK(mutr::execute(store, makeCmd("mget")).find("-ERR wrong number") == 0);

    CHECK(mutr::execute(store, makeCmd("NOPE")).find("-ERR unknown command 'NOPE'") == 0);
    std::string out;
    std::string inbound = respCommand({"GET"});
    CHECK(mutr::handleInput(store, inbound, out));
    CHECK(out.find("-ERR wrong number of arguments for 'get'") == 0);
    CHECK(inbound.empty());

    std::string inline_buf = "ping\r\nPING hi\r\n";
    std::string inline_out;
    CHECK(mutr::handleInput(store, inline_buf, inline_out));
    CHECK(inline_out == "+PONG\r\n" + mutr::respBulk("hi"));
    CHECK(inline_buf.empty());
}

void test_expiry() {
    using clock = std::chrono::steady_clock;
    mutr::Store store;
    const auto t0 = clock::time_point(std::chrono::seconds(1000));
    store.setNowForTest(t0);

    CHECK(mutr::execute(store, makeCmd("SET", {"k", "v", "ex", "1"})) == "+OK\r\n");
    store.setNowForTest(t0 + std::chrono::milliseconds(999));
    CHECK(mutr::execute(store, makeCmd("GET", {"k"})) == mutr::respBulk("v"));
    store.setNowForTest(t0 + std::chrono::milliseconds(1000));
    CHECK(mutr::execute(store, makeCmd("GET", {"k"})) == "$-1\r\n");
    CHECK(mutr::execute(store, makeCmd("EXISTS", {"k"})) == ":0\r\n");

    store.setNowForTest(t0);
    CHECK(mutr::execute(store, makeCmd("SET", {"p", "v", "PX", "50"})) == "+OK\r\n");
    store.setNowForTest(t0 + std::chrono::milliseconds(49));
    CHECK(mutr::execute(store, makeCmd("GET", {"p"})) == mutr::respBulk("v"));
    store.setNowForTest(t0 + std::chrono::milliseconds(50));
    CHECK(mutr::execute(store, makeCmd("GET", {"p"})) == "$-1\r\n");

    store.setNowForTest(t0);
    CHECK(mutr::execute(store, makeCmd("SET", {"e", "7", "EX", "1"})) == "+OK\r\n");
    CHECK(mutr::execute(store, makeCmd("INCR", {"e"})) == ":8\r\n");
    store.setNowForTest(t0 + std::chrono::milliseconds(999));
    CHECK(mutr::execute(store, makeCmd("GET", {"e"})) == mutr::respBulk("8"));
    store.setNowForTest(t0 + std::chrono::seconds(2));
    CHECK(mutr::execute(store, makeCmd("GET", {"e"})) == "$-1\r\n");

    store.setNowForTest(t0);
    CHECK(mutr::execute(store, makeCmd("SET", {"c", "old", "EX", "1"})) == "+OK\r\n");
    CHECK(mutr::execute(store, makeCmd("SET", {"c", "new"})) == "+OK\r\n");
    store.setNowForTest(t0 + std::chrono::seconds(5));
    CHECK(mutr::execute(store, makeCmd("GET", {"c"})) == mutr::respBulk("new"));

    CHECK(mutr::execute(store, makeCmd("SET", {"gone", "v"})) == "+OK\r\n");
    CHECK(mutr::execute(store, makeCmd("SET", {"gone", "v", "EX", "0"})) == "+OK\r\n");
    CHECK(mutr::execute(store, makeCmd("GET", {"gone"})) == "$-1\r\n");

    CHECK(mutr::execute(store, makeCmd("SET", {"keep", "yes"})) == "+OK\r\n");
    CHECK(mutr::execute(store, makeCmd("SET", {"keep", "no", "EX", "-1"})).find("-ERR invalid expire") == 0);
    CHECK(mutr::execute(store, makeCmd("GET", {"keep"})) == mutr::respBulk("yes"));
    CHECK(mutr::execute(store, makeCmd("SET", {"keep", "no", "EX", "nope"})).find("-ERR invalid expire") == 0);
    CHECK(mutr::execute(store, makeCmd("SET", {"keep", "no", "XX", "1"})).find("-ERR syntax error") == 0);
}

void test_writes() {
    int sv[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    const char* msg = "abc123";
    CHECK(mutr::writeAll(sv[0], msg, 6));
    char got[6] = {};
    std::size_t nread = 0;
    while (nread < 6) {
        const ssize_t n = ::recv(sv[1], got + nread, 6 - nread, 0);
        CHECK(n > 0);
        if (n <= 0) {
            break;
        }
        nread += static_cast<std::size_t>(n);
    }
    CHECK(std::string(got, nread) == "abc123");
    ::close(sv[0]);
    ::close(sv[1]);

    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    CHECK(mutr::setNonBlocking(sv[0]));
    CHECK(mutr::setNonBlocking(sv[1]));
    const std::string chunk(4096, 'x');
    bool blocked = false;
    std::size_t filled = 0;
    for (int i = 0; i < 100000; ++i) {
        const auto wrote = mutr::writeSome(sv[0], chunk.data(), chunk.size());
        if (wrote.kind == mutr::WriteKind::Blocked) {
            blocked = true;
            break;
        }
        CHECK(wrote.kind == mutr::WriteKind::Ok);
        CHECK(wrote.n > 0);
        filled += wrote.n;
    }
    CHECK(blocked);
    CHECK(filled > 0);
    const auto again = mutr::writeSome(sv[0], "y", 1);
    CHECK(again.kind == mutr::WriteKind::Blocked);
    // Linux unix-stream sockets account buffer space per skb. Reading one
    // byte does not free that skb, so a 1-byte send can stay EAGAIN until
    // the peer consumes a whole chunk. Drain until a 1-byte write fits.
    bool wrote_after_drain = false;
    for (int i = 0; i < 64 && !wrote_after_drain; ++i) {
        char buf[4096];
        const ssize_t n = ::recv(sv[1], buf, sizeof(buf), 0);
        CHECK(n > 0);
        if (n <= 0) {
            break;
        }
        const auto after = mutr::writeSome(sv[0], "y", 1);
        if (after.kind == mutr::WriteKind::Blocked) {
            continue;
        }
        CHECK(after.kind == mutr::WriteKind::Ok);
        CHECK(after.n == 1);
        wrote_after_drain = true;
    }
    CHECK(wrote_after_drain);
    ::close(sv[0]);
    ::close(sv[1]);

    flushStdio();
    const pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid < 0) {
        return;
    }
    if (pid == 0) {
        ::signal(SIGPIPE, SIG_DFL);
        ::alarm(2);
        int pair[2] = {-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
            _exit(3);
        }
        if (!mutr::setNoSigPipe(pair[0])) {
            _exit(4);
        }
        ::close(pair[1]);
        const bool ok = mutr::writeAll(pair[0], "hello", 5);
        _exit(ok ? 2 : 0);
    }
    int status = 0;
    CHECK(::waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
}

struct Child {
    pid_t pid = -1;
    ~Child() {
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            int status = 0;
            ::waitpid(pid, &status, 0);
        }
    }
};

std::string readLineFd(int fd, int timeout_ms) {
    std::string line;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (line.size() < 256) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now())
                              .count();
        if (left <= 0) {
            return {};
        }
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        const int rc = ::poll(&pfd, 1, static_cast<int>(left));
        if (rc <= 0) {
            return {};
        }
        char ch = 0;
        const ssize_t n = ::read(fd, &ch, 1);
        if (n <= 0) {
            return {};
        }
        if (ch == '\n') {
            return line;
        }
        line.push_back(ch);
    }
    return {};
}

bool readExact(int fd, char* dst, std::size_t n, int timeout_ms) {
    std::size_t got = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (got < n) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now())
                              .count();
        if (left <= 0) {
            return false;
        }
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        if (::poll(&pfd, 1, static_cast<int>(left)) <= 0) {
            return false;
        }
        const ssize_t k = ::recv(fd, dst + got, n - got, 0);
        if (k <= 0) {
            return false;
        }
        got += static_cast<std::size_t>(k);
    }
    return true;
}

bool expectExact(int fd, const std::string& want, int timeout_ms) {
    std::string got(want.size(), '\0');
    if (!readExact(fd, got.data(), got.size(), timeout_ms)) {
        return false;
    }
    return got == want;
}

int connectTcp(std::uint16_t port) {
    for (int attempt = 0; attempt < 50; ++attempt) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            return -1;
        }
        int on = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            return fd;
        }
        ::close(fd);
        ::poll(nullptr, 0, 20);
    }
    return -1;
}

std::uint16_t startServer(Child& child) {
    int sp[2] = {-1, -1};
    if (::pipe(sp) != 0) {
        std::cerr << "pipe failed\n";
        ++g_failed;
        return 0;
    }
    flushStdio();
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(sp[0]);
        ::close(sp[1]);
        ++g_failed;
        return 0;
    }
    if (pid == 0) {
        if (::dup2(sp[1], STDERR_FILENO) < 0) {
            _exit(127);
        }
        ::close(sp[0]);
        ::close(sp[1]);
        ::execl(g_bin.c_str(), "mutr", "--port", "0", "--threads", "1", static_cast<char*>(nullptr));
        _exit(127);
    }
    ::close(sp[1]);
    child.pid = pid;
    const std::string line = readLineFd(sp[0], 3000);
    ::close(sp[0]);
    constexpr std::string_view prefix = "listening ";
    if (line.size() < prefix.size() || line.compare(0, prefix.size(), prefix) != 0) {
        std::cerr << "bad listening line [" << line << "]\n";
        ++g_failed;
        return 0;
    }
    int port = 0;
    const auto res = std::from_chars(line.data() + prefix.size(), line.data() + line.size(), port);
    if (res.ec != std::errc() || res.ptr != line.data() + line.size() || port <= 0 || port > 65535) {
        std::cerr << "bad port in [" << line << "]\n";
        ++g_failed;
        return 0;
    }
    return static_cast<std::uint16_t>(port);
}

bool readBulk(int fd, std::string& value, int timeout_ms) {
    const std::string header = readLineFd(fd, timeout_ms);
    if (header.size() < 3 || header[0] != '$' || header.back() != '\r') {
        std::cerr << "bad bulk header [" << header << "]\n";
        return false;
    }
    int len = 0;
    const char* begin = header.data() + 1;
    const char* end = header.data() + header.size() - 1;
    const auto res = std::from_chars(begin, end, len);
    if (res.ec != std::errc() || res.ptr != end || len < 0) {
        std::cerr << "bad bulk length [" << header << "]\n";
        return false;
    }
    value.assign(static_cast<std::size_t>(len), '\0');
    if (len > 0 && !readExact(fd, value.data(), static_cast<std::size_t>(len), timeout_ms)) {
        return false;
    }
    char crlf[2] = {};
    return readExact(fd, crlf, 2, timeout_ms) && crlf[0] == '\r' && crlf[1] == '\n';
}

void test_live_server() {
    if (g_bin.empty() || ::access(g_bin.c_str(), X_OK) != 0) {
        std::cerr << "server binary not executable: " << g_bin << "\n";
        ++g_failed;
        return;
    }
    Child child;
    const std::uint16_t port = startServer(child);
    if (port == 0) {
        return;
    }

    {
        const int fd = connectTcp(port);
        CHECK(fd >= 0);
        if (fd < 0) {
            return;
        }
        const std::string cmd = respCommand({"PING"});
        for (char ch : cmd) {
            CHECK(mutr::writeAll(fd, &ch, 1));
        }
        CHECK(expectExact(fd, "+PONG\r\n", 2000));
        CHECK(mutr::writeAll(fd, "ping\r\n", 6));
        CHECK(expectExact(fd, "+PONG\r\n", 2000));
        const std::string with_arg = respCommand({"PING", "xyz"});
        CHECK(mutr::writeAll(fd, with_arg.data(), with_arg.size()));
        CHECK(expectExact(fd, mutr::respBulk("xyz"), 2000));

        std::string pipeline;
        for (int i = 0; i < 16; ++i) {
            pipeline += respCommand({"SET", "k" + std::to_string(i), "v" + std::to_string(i)});
        }
        for (std::size_t off = 0; off < pipeline.size();) {
            const std::size_t n = std::min<std::size_t>(pipeline.size() - off, 17);
            CHECK(mutr::writeAll(fd, pipeline.data() + off, n));
            off += n;
        }
        std::string oks(16 * 5, '\0');
        CHECK(readExact(fd, oks.data(), oks.size(), 3000));
        std::string expect_ok;
        for (int i = 0; i < 16; ++i) {
            expect_ok += "+OK\r\n";
        }
        CHECK(oks == expect_ok);

        std::string gets;
        for (int i = 0; i < 16; ++i) {
            gets += respCommand({"GET", "k" + std::to_string(i)});
        }
        CHECK(mutr::writeAll(fd, gets.data(), gets.size()));
        for (int i = 0; i < 16; ++i) {
            std::string value;
            CHECK(readBulk(fd, value, 2000));
            CHECK(value == "v" + std::to_string(i));
        }

        const std::string nulled("a\0b", 3);
        const std::string set_bin = respCommand({"SET", "nul", nulled});
        CHECK(mutr::writeAll(fd, set_bin.data(), set_bin.size()));
        CHECK(expectExact(fd, "+OK\r\n", 2000));
        const std::string get_bin = respCommand({"GET", "nul"});
        CHECK(mutr::writeAll(fd, get_bin.data(), get_bin.size()));
        std::string got_bin;
        CHECK(readBulk(fd, got_bin, 2000));
        CHECK(got_bin == nulled);

        std::string value(1024 * 1024, '\0');
        for (std::size_t i = 0; i < value.size(); ++i) {
            value[i] = static_cast<char>(i & 0xff);
        }
        const std::string set_big = respCommand({"SET", "big", value});
        for (std::size_t off = 0; off < set_big.size();) {
            const std::size_t n = std::min<std::size_t>(set_big.size() - off, 4096);
            CHECK(mutr::writeAll(fd, set_big.data() + off, n));
            off += n;
        }
        CHECK(expectExact(fd, "+OK\r\n", 5000));
        const std::string get_big = respCommand({"GET", "big"});
        CHECK(mutr::writeAll(fd, get_big.data(), get_big.size()));
        std::string got_big;
        CHECK(readBulk(fd, got_big, 5000));
        CHECK(got_big == value);

        const std::string bad = "*x\r\n";
        CHECK(mutr::writeAll(fd, bad.data(), bad.size()));
        const std::string err = readLineFd(fd, 2000);
        CHECK(err.size() >= 5);
        CHECK(err[0] == '-');
        char extra = 0;
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        CHECK(::poll(&pfd, 1, 2000) > 0);
        const ssize_t n = ::recv(fd, &extra, 1, 0);
        CHECK(n == 0);
        ::close(fd);
    }

    {
        const int fd = connectTcp(port);
        CHECK(fd >= 0);
        if (fd < 0) {
            return;
        }
        const std::string ping = respCommand({"PING"});
        CHECK(mutr::writeAll(fd, ping.data(), ping.size()));
        CHECK(expectExact(fd, "+PONG\r\n", 2000));
        const std::string set = respCommand({"SET", "persist", "yes"});
        CHECK(mutr::writeAll(fd, set.data(), set.size()));
        CHECK(expectExact(fd, "+OK\r\n", 2000));
        ::close(fd);
    }
    {
        const int fd = connectTcp(port);
        CHECK(fd >= 0);
        if (fd < 0) {
            return;
        }
        const std::string get = respCommand({"GET", "persist"});
        CHECK(mutr::writeAll(fd, get.data(), get.size()));
        std::string value;
        CHECK(readBulk(fd, value, 2000));
        CHECK(value == "yes");
        const std::string incr = respCommand({"INCR", "counter"});
        CHECK(mutr::writeAll(fd, incr.data(), incr.size()));
        CHECK(expectExact(fd, ":1\r\n", 2000));
        const std::string mset = respCommand({"MSET", "aa", "1", "bb", "2"});
        CHECK(mutr::writeAll(fd, mset.data(), mset.size()));
        CHECK(expectExact(fd, "+OK\r\n", 2000));
        const std::string mget = respCommand({"MGET", "aa", "zz", "bb"});
        CHECK(mutr::writeAll(fd, mget.data(), mget.size()));
        CHECK(expectExact(fd, "*3\r\n$1\r\n1\r\n$-1\r\n$1\r\n2\r\n", 2000));
        const std::string del = respCommand({"DEL", "aa", "missing"});
        CHECK(mutr::writeAll(fd, del.data(), del.size()));
        CHECK(expectExact(fd, ":1\r\n", 2000));
        const std::string exists = respCommand({"EXISTS", "bb", "aa"});
        CHECK(mutr::writeAll(fd, exists.data(), exists.size()));
        CHECK(expectExact(fd, ":1\r\n", 2000));
        ::close(fd);
    }
}

void test_output_budget() {
    mutr::Store store;
    mutr::Limits limits;
    // One PING request is 14 bytes, so the cap must allow a command through.
    // Replies are 7 bytes; 40 stops the batch before every PING is answered.
    limits.max_buffer = 40;
    std::string pending;
    for (int i = 0; i < 12; ++i) {
        pending += respCommand({"PING"});
    }
    std::string all;
    int rounds = 0;
    while (!pending.empty() && rounds < 10) {
        std::string out;
        CHECK(mutr::handleInput(store, pending, out, limits));
        CHECK(!out.empty());
        all += out;
        ++rounds;
    }
    CHECK(pending.empty());
    CHECK(rounds > 1);
    std::string expect;
    for (int i = 0; i < 12; ++i) {
        expect += "+PONG\r\n";
    }
    CHECK(all == expect);
}

void test_level_triggered() {
    mutr::EventLoop loop;
    CHECK(loop.ok());
    int sv[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    CHECK(mutr::setNonBlocking(sv[0]));
    CHECK(mutr::setNonBlocking(sv[1]));
    CHECK(loop.add(sv[0], true, false));

    auto ev = loop.wait(30);
    CHECK(ev.empty());
    CHECK(::send(sv[1], "hi", 2, 0) == 2);
    ev = loop.wait(200);
    CHECK(ev.size() == 1);
    CHECK(ev[0].fd == sv[0]);
    CHECK(ev[0].readable);
    ev = loop.wait(200);
    CHECK(ev.size() == 1);
    CHECK(ev[0].readable);

    char tmp[8] = {};
    CHECK(::recv(sv[0], tmp, sizeof(tmp), 0) == 2);
    ev = loop.wait(30);
    CHECK(ev.empty());

    CHECK(loop.update(sv[0], true, true));
    ev = loop.wait(200);
    bool writable = false;
    for (const auto& fired : ev) {
        if (fired.fd == sv[0] && fired.writable) {
            writable = true;
        }
    }
    CHECK(writable);
    ev = loop.wait(200);
    writable = false;
    for (const auto& fired : ev) {
        if (fired.writable) {
            writable = true;
        }
    }
    CHECK(writable);
    CHECK(loop.update(sv[0], true, false));
    ev = loop.wait(30);
    for (const auto& fired : ev) {
        CHECK(!fired.writable);
    }
    ::close(sv[0]);
    ::close(sv[1]);
}

void test_threads_flag() {
    int sp[2] = {-1, -1};
    CHECK(::pipe(sp) == 0);
    flushStdio();
    const pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid < 0) {
        return;
    }
    if (pid == 0) {
        if (::dup2(sp[1], STDERR_FILENO) < 0) {
            _exit(127);
        }
        ::close(sp[0]);
        ::close(sp[1]);
        ::execl(g_bin.c_str(), "mutr", "--port", "0", "--threads", "2", static_cast<char*>(nullptr));
        _exit(127);
    }
    ::close(sp[1]);
    int status = 0;
    CHECK(::waitpid(pid, &status, 0) == pid);
    std::string err;
    char buf[256];
    while (true) {
        const ssize_t n = ::read(sp[0], buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        err.append(buf, static_cast<std::size_t>(n));
    }
    ::close(sp[0]);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 2);
    CHECK(err.find("only --threads 1") != std::string::npos);
}

void test_concurrent_connections() {
    if (g_bin.empty() || ::access(g_bin.c_str(), X_OK) != 0) {
        std::cerr << "server binary not executable: " << g_bin << "\n";
        ++g_failed;
        return;
    }
    Child child;
    const std::uint16_t port = startServer(child);
    if (port == 0) {
        return;
    }
    constexpr int kClients = 64;
    std::vector<int> fds;
    fds.reserve(kClients);
    for (int i = 0; i < kClients; ++i) {
        const int fd = connectTcp(port);
        if (fd < 0) {
            CHECK(fd >= 0);
            break;
        }
        fds.push_back(fd);
    }
    CHECK(static_cast<int>(fds.size()) == kClients);

    const std::string partial = "*1\r\n$4\r\nPI";
    for (int fd : fds) {
        CHECK(mutr::writeAll(fd, partial.data(), partial.size()));
    }
    for (int fd : fds) {
        CHECK(mutr::writeAll(fd, "NG\r\n", 4));
    }
    for (int fd : fds) {
        CHECK(expectExact(fd, "+PONG\r\n", 2000));
    }

    for (int i = 0; i < static_cast<int>(fds.size()); ++i) {
        const std::string key = "c" + std::to_string(i);
        const std::string value = "v" + std::to_string(i);
        std::string req = respCommand({"SET", key, value});
        req += respCommand({"GET", key});
        CHECK(mutr::writeAll(fds[static_cast<std::size_t>(i)], req.data(), req.size()));
    }
    for (int i = 0; i < static_cast<int>(fds.size()); ++i) {
        CHECK(expectExact(fds[static_cast<std::size_t>(i)], "+OK\r\n", 2000));
        std::string value;
        CHECK(readBulk(fds[static_cast<std::size_t>(i)], value, 2000));
        CHECK(value == "v" + std::to_string(i));
    }
    for (int fd : fds) {
        ::close(fd);
    }
}

void test_shard_distribution() {
    mutr::Store store(64);
    const std::string sample = "alpha";
    const auto hash = mutr::Store::hashKey(sample);
    CHECK(store.shardIndex(sample) == static_cast<std::size_t>(hash >> (64 - 6)));
    CHECK(store.shardIndex(sample) < 64);

    bool high_differs_from_low = false;
    for (int i = 0; i < 1000; ++i) {
        const std::string key = "k" + std::to_string(i);
        const auto h = mutr::Store::hashKey(key);
        const auto high = static_cast<std::size_t>(h >> 58);
        const auto low = static_cast<std::size_t>(h & 63);
        if (high == low) {
            continue;
        }
        high_differs_from_low = true;
        CHECK(store.shardIndex(key) == high);
        CHECK(store.shardIndex(key) != low);
        break;
    }
    CHECK(high_differs_from_low);

    std::array<int, 64> counts{};
    for (int i = 0; i < 6400; ++i) {
        const std::size_t shard = store.shardIndex("key-" + std::to_string(i));
        CHECK(shard < counts.size());
        if (shard < counts.size()) {
            ++counts[shard];
        }
    }
    int used = 0;
    for (int count : counts) {
        if (count > 0) {
            ++used;
        }
    }
    CHECK(used == 64);

    mutr::Store one(1);
    CHECK(one.shardCount() == 1);
    CHECK(one.shardIndex("a") == 0);
    CHECK(one.shardIndex("other") == 0);
}

void test_incr_atomicity() {
    mutr::Store store(64);
    constexpr int kThreads = 8;
    constexpr int kEach = 1000;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&store] {
            for (int n = 0; n < kEach; ++n) {
                store.incr("same");
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    const auto value = store.get("same");
    CHECK(value.has_value());
    CHECK(value && *value == std::to_string(kThreads * kEach));
}

void test_expiry_under_lock() {
    mutr::Store store(32);
    const auto t0 = std::chrono::steady_clock::time_point{};
    store.setNowForTest(t0);
    CHECK(store.set("same", "10", std::chrono::milliseconds(5)));
    store.setNowForTest(t0 + std::chrono::milliseconds(5));

    constexpr int kThreads = 8;
    constexpr int kEach = 100;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&store] {
            for (int n = 0; n < kEach; ++n) {
                store.incr("same");
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    const auto value = store.get("same");
    CHECK(value.has_value());
    // The expired "10" must not survive into the increment. 8*100 starts at 0.
    CHECK(value && *value == std::to_string(kThreads * kEach));
}

void test_mset_snapshot() {
    mutr::Store store(64);
    std::string a;
    std::string b;
    for (int i = 0; i < 10000 && b.empty(); ++i) {
        const std::string key = "k" + std::to_string(i);
        if (a.empty()) {
            a = key;
        } else if (store.shardIndex(key) != store.shardIndex(a)) {
            b = key;
        }
    }
    CHECK(!a.empty());
    CHECK(!b.empty());
    CHECK(store.shardIndex(a) != store.shardIndex(b));
    store.mset({a, "1", b, "2"});

    std::atomic<int> bad{0};
    auto worker = [&](bool flipped) {
        for (int i = 0; i < 2000; ++i) {
            if (flipped) {
                store.mset({b, "1", a, "2"});
            } else {
                store.mset({a, "1", b, "2"});
            }
            const auto got = store.mget({a, b});
            if (got.size() != 2 || !got[0] || !got[1]) {
                bad.fetch_add(1);
                continue;
            }
            const bool first = *got[0] == "1" && *got[1] == "2";
            const bool second = *got[0] == "2" && *got[1] == "1";
            if (!first && !second) {
                bad.fetch_add(1);
            }
        }
    };
    std::thread left(worker, false);
    std::thread right(worker, true);
    left.join();
    right.join();
    CHECK(bad.load() == 0);
}

void test_shards_flag() {
    if (g_bin.empty() || ::access(g_bin.c_str(), X_OK) != 0) {
        std::cerr << "server binary not executable: " << g_bin << "\n";
        ++g_failed;
        return;
    }
    int sp[2] = {-1, -1};
    CHECK(::pipe(sp) == 0);
    flushStdio();
    const pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid < 0) {
        return;
    }
    if (pid == 0) {
        if (::dup2(sp[1], STDERR_FILENO) < 0) {
            _exit(127);
        }
        ::close(sp[0]);
        ::close(sp[1]);
        ::execl(g_bin.c_str(), "mutr", "--port", "0", "--shards", "3", static_cast<char*>(nullptr));
        _exit(127);
    }
    ::close(sp[1]);
    int status = 0;
    CHECK(::waitpid(pid, &status, 0) == pid);
    std::string err;
    char buf[256];
    while (true) {
        const ssize_t n = ::read(sp[0], buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        err.append(buf, static_cast<std::size_t>(n));
    }
    ::close(sp[0]);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 2);
    CHECK(err.find("invalid shards") != std::string::npos);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2) {
        g_bin = argv[1];
    } else {
        g_bin = "./mutr";
    }

    std::cout << "RUN test_fragmentation_and_pipeline\n";
    test_fragmentation_and_pipeline();
    std::cout << "RUN test_random_and_large_splits\n";
    test_random_and_large_splits();
    std::cout << "RUN test_malformed_does_not_throw\n";
    test_malformed_does_not_throw();
    std::cout << "RUN test_commands\n";
    test_commands();
    std::cout << "RUN test_expiry\n";
    test_expiry();
    std::cout << "RUN test_writes\n";
    test_writes();
    std::cout << "RUN test_live_server\n";
    test_live_server();
    std::cout << "RUN test_output_budget\n";
    test_output_budget();
    std::cout << "RUN test_level_triggered\n";
    test_level_triggered();
    std::cout << "RUN test_threads_flag\n";
    test_threads_flag();
    std::cout << "RUN test_concurrent_connections\n";
    test_concurrent_connections();
    std::cout << "RUN test_shard_distribution\n";
    test_shard_distribution();
    std::cout << "RUN test_incr_atomicity\n";
    test_incr_atomicity();
    std::cout << "RUN test_expiry_under_lock\n";
    test_expiry_under_lock();
    std::cout << "RUN test_mset_snapshot\n";
    test_mset_snapshot();
    std::cout << "RUN test_shards_flag\n";
    test_shards_flag();

    if (g_failed != 0) {
        std::cout << "failed " << g_failed << " checks\n";
        return 1;
    }
    std::cout << "all checks passed\n";
    return 0;
}
