# mutr

mutr is a RESP2 key-value server written in C++17. Each worker thread runs its own event loop, epoll on Linux and kqueue on macOS, and the store is sharded. The commands implemented in `src/commands.cpp` are PING, SET, GET, DEL, EXISTS, INCR, MSET, and MGET. Names are matched case-insensitively. SET takes a key and a value, or the same pair plus `EX` seconds or `PX` milliseconds. An expiry of 0 deletes the key. There is no background scan; an expired key is removed the next time a command touches it.

## Headline results

These three rows are mutr 4×64 against Redis 6.2 with io-threads 4. The paired ratio is mutr ops/sec divided by Redis ops/sec on the same replicate (min / median / max). Ops/sec and p99 are medians across the five replicates. Figures are copied from `bench/results/headline.md`.

| workload | paired ratio min / median / max | mutr 4×64 median ops/sec | mutr 4×64 p99 ms | Redis io-4 median ops/sec | Redis io-4 p99 ms |
| --- | --- | ---: | ---: | ---: | ---: |
| 64 B SET, P16, 128 connections | 2.17 / 2.48 / 2.97 | 2510616.23 | 2.023 | 845437.91 | 3.231 |
| 64 B GET, P16, 32 connections | 2.25 / 2.57 / 2.64 | 2209674.11 | 0.495 | 839718.62 | 0.783 |
| 1024 B SET, P16, 128 connections | 2.51 / 2.73 / 4.18 | 1354689.41 | 3.263 | 401180.37 | 10.111 |

mutr 4×1 / mutr 4×64 paired-ratio medians at pipeline 16, 64 B values: SET 32 connections 0.23, SET 128 connections 0.22, SET 512 connections 0.23, GET 32 connections 0.23, GET 128 connections 0.24, GET 512 connections 0.23.

The full matrix is in [docs/BENCHMARKS.md](docs/BENCHMARKS.md). Raw rows are in [bench/results/runs.csv](bench/results/runs.csv).

## Limitations

- At low connection counts without pipelining, mutr 4×64 loses to Redis 6.2. The clean-cell paired ratio (median per workload, mutr 4×64 / Redis io-4, every replicate valid and not client-bound) ranges from 0.78 to 2.94. Single-threaded mutr is slower than Redis 1 at pipeline 1 once there are more than 8 connections: the paired ratio is about 0.75 to 0.87. At 8 connections and pipeline 1 the within-replicate ratio is about 0.90 to 0.95.
- The benchmarks ran in a Lima VM on a laptop. The server was pinned to 4 vCPUs and memtier to the other 4. Host speed drifted over the multi-day run, so comparisons are paired within a replicate.
- Many mutr 4×64 cells were client-bound (20 of the 21 flagged cells). Those ratios are lower bounds.
- The comparison server is Redis 6.2.20. Its io-threads only parallelize socket I/O. This is not a newer Redis.
- Only the commands listed above exist. There is no persistence, no replication, and no active expiry.
- memtier is closed-loop, so queueing delay under overload is not captured.
- Sanitizer coverage is Clang TSan, ASan, and UBSan at 1, 2, 4, and 8 workers. GCC TSan is unusable on aarch64 in the benchmark VM. Paths not tested: `--pin`, injected EINTR, a full handoff queue, and epoll returning more than 64 ready fds.

## Architecture

One process. `--threads N` starts N worker threads. The main thread accepts and does not run commands, so `--threads 1` is still two operating-system threads. The listen socket is not `SO_REUSEPORT`. The acceptor hands each new connection to a worker round-robin, through a mutex-protected queue and a non-blocking pipe, and does not touch that fd again. The queue is not capped.

Each worker has its own level-triggered loop: epoll without `EPOLLET`, kqueue without `EV_CLEAR`. Level-triggered reporting is what makes the handoff correct. Bytes can arrive after `accept` and before the worker registers the fd, and a level-triggered loop still reports the fd. Write interest is registered only while that worker has unsent bytes.

The default store is 64 shards. `--shards` must be a power of two from 1 through 1048576. Each shard is an `alignas(64)` object with its own `std::unordered_map` and its own `std::mutex`, so neighboring mutexes do not share a cache line. The shard index is the high bits of a 64-bit FNV-1a hash followed by a splitmix64 finalizer. One shard maps every key to index 0 and does not shift by 64. A single-key command locks that shard for the whole operation. MSET, MGET, and variadic DEL and EXISTS lock every shard they touch, in ascending shard-index order, and hold those locks until the command finishes.

`parseOne` reads one command from the front of the buffer. Until the command is complete it returns `NeedMoreData` and consumes nothing.

The rest of the threading, shutdown, and expiry behavior is in [docs/DESIGN.md](docs/DESIGN.md).

## Build and run

`CMakePresets.json` has four presets: `release`, `asan`, `ubsan`, and `tsan`. There is no `debug` preset. `release` is a Release build (`-O3 -DNDEBUG`). The three sanitizer presets are Debug builds (`-g -O0`) with that sanitizer enabled. ASan and TSan cannot be combined. On this macOS toolchain ASan and TSan do not run; UBSan does. On aarch64 Linux, GCC's TSan is not usable; configure that preset with `clang++`.

```
cmake --preset release
cmake --build --preset release
```

The same two commands with `asan`, `ubsan`, or `tsan` select those presets. The binary is `build/<preset>/mutr`.

```
./build/release/mutr --port 6379 --threads 4 --shards 64
```

`--port` defaults to 6379 (`0` lets the kernel pick). `--threads` defaults to 1. `--shards` defaults to 64. `--verbose` logs accepts, closes, and one stats line on shutdown. `--pin` pins each worker with `sched_setaffinity` on Linux and is a no-op elsewhere. `--help` prints the same flags.

Tests:

```
ctest --test-dir build/release --output-on-failure
```

`ctest` runs `mutr_tests` and passes it the `mutr` binary. `MUTR_TEST_THREADS` sets the worker count for live-server tests that do not pass their own count. The CI matrix uses 1, 2, 4, and 8.

The timed matrix is `python3 bench/run_matrix.py`. It does not run in CI. It expects three binaries at fixed paths: `/tmp/mutr-bench/mutr`, `/tmp/redis-6.2.20/src/redis-server`, and `/tmp/memtier_benchmark/memtier_benchmark`. `bench/results/env.txt` records the mutr configure line as `cmake -S <repo> -B /tmp/mutr-bench -DCMAKE_BUILD_TYPE=Release -G Ninja` and the compile flags `-O3 -DNDEBUG -std=c++17 -Wall -Wextra`. It does not record a separate compile command. Redis 6.2.20 is the GitHub tag tarball `https://github.com/redis/redis/archive/refs/tags/6.2.20.tar.gz`, built from `/tmp/redis-6.2.20` with `make -j8`. memtier is `https://github.com/RedisLabs/memtier_benchmark.git` at commit `f8d4458d8b685cf7061e139508f936feb2343281`, built with `autoreconf -ivf && ./configure && make -j8`. The correctness check is `python3 bench/correctness.py`.

## CI

[![CI](https://github.com/ArshAnan/mutr/actions/workflows/ci.yml/badge.svg)](https://github.com/ArshAnan/mutr/actions/workflows/ci.yml)
