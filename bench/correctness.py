#!/usr/bin/env python3
"""Send one command sequence to mutr and Redis and compare raw replies.

Exits 0 only when every reply is byte-for-byte identical and none of the
replies is a RESP error. Starts both servers itself.
"""

import os
import signal
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def results_dir():
    override = os.environ.get("MUTR_BENCH_OUT")
    if override:
        return override
    candidate = os.path.join(ROOT, "bench", "results")
    try:
        os.makedirs(candidate, exist_ok=True)
        probe = os.path.join(candidate, ".write_probe")
        with open(probe, "w", encoding="utf-8") as fh:
            fh.write("ok")
        os.remove(probe)
        return candidate
    except OSError:
        return "/var/tmp/mutr-bench-results"


OUT = os.path.join(results_dir(), "correctness.txt")
MUTR = "/tmp/mutr-bench/mutr"
REDIS = "/tmp/redis-6.2.20/src/redis-server"
REDIS_CLI = "/tmp/redis-6.2.20/src/redis-cli"


def encode(parts):
    out = f"*{len(parts)}\r\n".encode()
    for part in parts:
        if isinstance(part, str):
            part = part.encode()
        out += f"${len(part)}\r\n".encode() + part + b"\r\n"
    return out


def read_reply(sock):
    def line():
        buf = b""
        while b"\n" not in buf:
            chunk = sock.recv(1)
            if not chunk:
                raise SystemExit("short read")
            buf += chunk
        return buf

    header = line()
    kind = header[:1]
    if kind in (b"+", b"-", b":"):
        return header
    if kind == b"$":
        n = int(header[1:].strip())
        if n < 0:
            return header
        body = b""
        need = n + 2
        while len(body) < need:
            chunk = sock.recv(need - len(body))
            if not chunk:
                raise SystemExit("short bulk")
            body += chunk
        return header + body
    if kind == b"*":
        n = int(header[1:].strip())
        body = header
        for _ in range(n):
            body += read_reply(sock)
        return body
    raise SystemExit("bad reply header " + repr(header))


def start(cmd, ready_path, needle):
    # Redis logs "Ready to accept connections" on stdout. mutr logs on stderr.
    log = open(ready_path, "wb", buffering=0)
    proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT)
    deadline = time.time() + 15
    while time.time() < deadline:
        if proc.poll() is not None:
            sys.stderr.write(open(ready_path, "rb").read().decode())
            raise SystemExit(f"server exited {proc.returncode}: {cmd}")
        data = open(ready_path, "rb").read()
        if needle.encode() in data:
            return proc
        time.sleep(0.05)
    proc.send_signal(signal.SIGTERM)
    raise SystemExit("server did not become ready: " + repr(open(ready_path, "rb").read()))


def commands():
    value64 = bytes([i % 256 for i in range(64)])
    value_nul = b"a\x00b\xffc"
    seq = [
        [b"PING"],
        [b"ping"],
        [b"SET", b"foo", b"bar"],
        [b"GET", b"foo"],
        [b"GET", b"missing"],
        [b"INCR", b"n"],
        [b"INCR", b"n"],
        [b"SET", b"bin", value64],
        [b"GET", b"bin"],
        [b"SET", b"nul", value_nul],
        [b"GET", b"nul"],
        [b"MSET", b"a", b"1", b"b", b"2"],
        [b"MGET", b"a", b"b", b"c"],
        [b"DEL", b"a"],
        [b"EXISTS", b"a", b"b"],
        [b"DEL", b"a", b"a"],
    ]
    return seq


def run_sequence(port):
    sock = socket.create_connection(("127.0.0.1", port))
    sock.settimeout(5)
    replies = []
    for parts in commands():
        sock.sendall(encode(parts))
        replies.append(read_reply(sock))
    # One pipelined write of five PINGs, then five replies.
    sock.sendall(encode([b"PING"]) * 5)
    for _ in range(5):
        replies.append(read_reply(sock))
    sock.close()
    return replies


def main():
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    # Inherit a raised nofile from the caller when it set one.
    mutr = start(
        ["taskset", "-c", "0-3", MUTR, "--port", "17101", "--threads", "2", "--shards", "64"],
        "/tmp/correct-mutr.err",
        "listening 17101",
    )
    try:
        redis = start(
            [
                "taskset", "-c", "0-3", REDIS,
                "--port", "17102", "--bind", "127.0.0.1",
                "--save", "", "--appendonly", "no", "--protected-mode", "no",
                "--daemonize", "no",
            ],
            "/tmp/correct-redis.err",
            "Ready to accept",
        )
    except BaseException:
        mutr.send_signal(signal.SIGTERM)
        mutr.wait(timeout=5)
        raise
    try:
        cfg = subprocess.check_output(
            [REDIS_CLI, "-p", "17102", "CONFIG", "GET", "maxmemory"], text=True
        )
        cfg += subprocess.check_output(
            [REDIS_CLI, "-p", "17102", "CONFIG", "GET", "appendonly"], text=True
        )
        cfg += subprocess.check_output(
            [REDIS_CLI, "-p", "17102", "CONFIG", "GET", "save"], text=True
        )
        left = run_sequence(17101)
        right = run_sequence(17102)
    finally:
        mutr.send_signal(signal.SIGTERM)
        redis.send_signal(signal.SIGTERM)
        mutr.wait(timeout=5)
        redis.wait(timeout=5)

    lines = ["redis CONFIG GET (maxmemory, appendonly, save):", cfg.rstrip(), ""]
    bad = 0
    if len(left) != len(right):
        lines.append(f"reply count mutr={len(left)} redis={len(right)}")
        bad += 1
    for i, (a, b) in enumerate(zip(left, right)):
        err = a.startswith(b"-") or b.startswith(b"-")
        same = a == b
        lines.append(f"{i} same={same} error={err} mutr={a!r} redis={b!r}")
        if not same or err:
            bad += 1
    lines.append(f"mismatches_or_errors {bad}")
    text = "\n".join(lines) + "\n"
    with open(OUT, "w") as fh:
        fh.write(text)
    sys.stdout.write(text)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
