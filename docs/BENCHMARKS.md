# Benchmarks

Timed memtier results for mutr against Redis 6.2.20. The raw rows are `bench/results/runs.csv` (1,440 cells). The machine, compiler, binaries, and pin layout are in `bench/results/env.txt`. A separate correctness check, one command sequence against both servers, is in `bench/results/correctness.txt`: 21 replies, 0 mismatches.

mutr in this run is commit `37c93c68bb0c7746eaad562059b28286f24d300e`, Release, `-O3 -DNDEBUG`. Redis is 6.2.20 built from the GitHub tag tarball. memtier is `f8d4458d8b685cf7061e139508f936feb2343281` with `--protocol=redis`.

## Setup

The runs are inside a Lima VM: 8 vCPUs, 8 GiB, on an Apple M4 MacBook Pro. The server process is pinned to vCPUs 0–3 and memtier to 4–7 on every cell, including the 1-thread servers, so the client does not get extra cores when the server is small. mutr `--pin` is off. `--threads 4` is 4 workers plus the acceptor, so 5 threads on 4 cores. `--threads 8` is not in the matrix.

Each cell starts a fresh server. GET and mixed preload keys 1..1,000,000. A 10 second warmup is discarded. The recorded run is 30 seconds. Every valid GET and mixed row has `set_count_preload` 1000000 and `misses_per_sec` 0. Every valid row has `connection_errors` 0.

Six server configurations:

| Label | Process |
| --- | --- |
| mutr 1×64 | `mutr --threads 1 --shards 64` |
| Redis 1 | `redis-server`, one command thread, no io-threads |
| mutr 2×64 | `mutr --threads 2 --shards 64` |
| Redis io-4 | Redis with `--io-threads 4 --io-threads-do-reads yes` |
| mutr 4×64 | `mutr --threads 4 --shards 64` |
| mutr 4×1 | `mutr --threads 4 --shards 1` |

Redis io-4 still runs each command on the main thread. The extra threads read and write sockets. mutr 4×1 is the same 4 workers as mutr 4×64 with one shard mutex for every key.

The workload grid is value size 64 and 1024, mix SET, GET, and 1:1 mixed, connections 8, 32, 128, and 512, pipeline 1 and 16, five replicates. memtier always uses 4 threads, so 8 connections is `-c 2` and 512 connections is `-c 128`.

## How to read the numbers

A table cell is the median ops/sec of the valid replicates, rounded to the nearest op/sec. 1,438 of 1,440 rows are `valid=1`. The two excluded rows are both replicate 1 of SET, 1024 byte values, 512 connections, pipeline 16: mutr 1×64 and mutr 2×64, `invalid_reason=warmup_exit_1`. Those two table cells are marked † and use the other four replicates.

Absolute ops/sec moved between replicates. Across the 288 workload×server cells, the median of (fastest replicate / slowest replicate) is 1.94. Replicate 1 is the fast one more often than the others, and replicate 4 is the slow one more often. That fits a laptop VM whose host speed is not constant across the multi-day run. A comparison stated in the text is the median of the ratio taken inside each replicate, so a slow replicate of mutr is paired with the Redis run from that same replicate.

`client_bound=1` means a memtier thread reported `cores_used` ≥ 0.95, or mpstat showed under 5% idle on one of vCPUs 4–7. The flag is a client ceiling. The row can still be `valid=1`. An asterisk means at least one of the replicates for that cell was client-bound. 71 rows in 21 cells are flagged. Twenty of those cells are mutr 4×64. The other is Redis io-4, GET, 64 byte values, 128 connections, pipeline 1 (1 of 5 replicates).

## One thread

At 8 connections and pipeline 1, mutr 1×64 and Redis 1 stay close. The within-replicate ops ratio is about 0.90 to 0.95, and both servers sit near 99% of one core.

With more connections and pipeline 1, mutr 1×64 falls behind Redis 1. The paired ratio is about 0.75 to 0.87. `--threads 1` is still two operating-system threads: the acceptor and one worker, sharing the server's four cores with the worker doing the commands.

With pipeline 16 the paired ratio is usually about 0.91 to 1.11. Two GET 1024 byte rows sit higher (about 1.20 to 1.33 at 32, 128, and 512 connections).

## Two and four threads

At 8 connections and pipeline 1, extra mutr threads do not raise throughput. Two threads is about the same as one (paired ratio about 0.94 to 1.22). Four threads is slower (about 0.74 to 0.94). Eight connections do not fill four workers, and the acceptor shares cores 0–3 with them.

From 32 connections up, two mutr threads is about twice the 1-thread mutr rate. Four threads is about 2.3× to 4.4× that rate. The larger multiples are the pipeline 16 rows.

Redis io-4 does not beat Redis 1 at 8 connections (paired ratio about 0.95 to 1.07). At 128 and 512 connections it does, more on GET and mixed (often about 1.4× to 2.0×) than on SET. mutr 4×64 is faster than Redis io-4 on those same busier cells: paired ratio about 1.5× to 2.8×, and the pipeline 16 rows cluster around 2.2× to 2.8×. Several of those mutr 4×64 cells are client-bound, so the table is the client ceiling there, and the server gap can be wider than the asterisked number.

## One shard against 64

mutr 4×1 and mutr 4×64 match at 8 connections and pipeline 1 (paired ratio about 0.95 to 0.98). With pipeline 16 and 32 or more connections, one shard delivers about 0.22 to 0.31 of the 64-shard rate. On 64 byte values the median of that ratio sits around 0.22 to 0.26. Every key takes the same mutex, so the extra workers queue on one lock. Sixty-four shards is the configuration where those workers run at once.

## Median ops/sec

Asterisk: at least one replicate was client-bound. Dagger: median of four replicates; replicate 1 of that cell failed warmup.

### SET, 64 byte values

| connections | pipeline | mutr 1×64 | Redis 1 | mutr 2×64 | Redis io-4 | mutr 4×64 | mutr 4×1 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 8 | 1 | 108,000 | 111,494 | 110,402 | 107,233 | 94,162 | 92,000 |
| 8 | 16 | 475,315 | 471,156 | 1,021,717 | 471,847 | 981,593 | 423,798 |
| 32 | 1 | 122,785 | 160,428 | 239,286 | 159,854 | 402,210 | 262,238 |
| 32 | 16 | 517,995 | 909,787 | 1,607,401 | 703,398 | 1,887,200 | 486,501 |
| 128 | 1 | 236,867 | 315,040 | 493,555 | 333,002 | 551,953* | 299,329 |
| 128 | 16 | 887,339 | 978,863 | 1,732,982 | 845,438 | 2,510,616 | 599,131 |
| 512 | 1 | 232,618 | 294,766 | 462,141 | 372,650 | 658,589* | 357,652 |
| 512 | 16 | 736,969 | 782,214 | 1,335,502 | 981,922 | 2,554,137 | 610,710 |

### GET, 64 byte values

| connections | pipeline | mutr 1×64 | Redis 1 | mutr 2×64 | Redis io-4 | mutr 4×64 | mutr 4×1 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 8 | 1 | 194,693 | 205,367 | 177,486 | 141,213 | 127,316 | 137,169 |
| 8 | 16 | 1,000,114 | 1,021,612 | 1,848,213 | 959,064 | 1,531,815 | 708,551 |
| 32 | 1 | 148,837 | 172,439 | 443,106 | 200,082 | 467,670* | 321,989 |
| 32 | 16 | 522,947 | 478,008 | 1,266,475 | 839,719 | 2,209,674 | 506,839 |
| 128 | 1 | 139,752 | 180,732 | 275,746 | 288,643* | 498,776* | 335,067 |
| 128 | 16 | 535,675 | 478,457 | 1,312,556 | 927,275 | 2,194,788 | 504,873 |
| 512 | 1 | 124,096 | 169,872 | 269,899 | 325,440 | 482,892* | 345,648 |
| 512 | 16 | 584,048 | 590,454 | 1,204,012 | 867,240 | 2,197,154 | 505,993 |

### Mixed 1:1, 64 byte values

| connections | pipeline | mutr 1×64 | Redis 1 | mutr 2×64 | Redis io-4 | mutr 4×64 | mutr 4×1 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 8 | 1 | 112,741 | 117,746 | 115,276 | 117,856 | 95,071 | 93,463 |
| 8 | 16 | 439,597 | 431,795 | 1,034,477 | 446,723 | 995,293 | 433,666 |
| 32 | 1 | 132,989 | 166,888 | 258,988 | 182,971 | 462,533* | 283,361 |
| 32 | 16 | 450,922 | 473,581 | 1,130,909 | 736,236 | 1,911,734 | 435,721 |
| 128 | 1 | 131,393 | 174,093 | 265,475 | 258,294 | 487,614* | 296,889 |
| 128 | 16 | 443,277 | 462,352 | 1,116,698 | 811,143 | 1,944,068 | 430,101 |
| 512 | 1 | 120,434 | 138,488 | 264,673 | 299,617 | 478,877* | 302,191 |
| 512 | 16 | 456,093 | 495,479 | 1,100,059 | 787,245 | 1,961,089 | 436,067 |

### SET, 1024 byte values

| connections | pipeline | mutr 1×64 | Redis 1 | mutr 2×64 | Redis io-4 | mutr 4×64 | mutr 4×1 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 8 | 1 | 82,830 | 96,484 | 101,708 | 103,694 | 83,139 | 82,149 |
| 8 | 16 | 333,019 | 315,379 | 650,597 | 336,772 | 815,212 | 324,593 |
| 32 | 1 | 118,379 | 146,067 | 243,955 | 139,714 | 316,301 | 213,782 |
| 32 | 16 | 318,157 | 331,646 | 798,920 | 512,773 | 1,437,642 | 353,530 |
| 128 | 1 | 126,390 | 159,486 | 249,992 | 217,626 | 409,718* | 265,023 |
| 128 | 16 | 301,575 | 288,599 | 769,161 | 401,180 | 1,354,689 | 334,559 |
| 512 | 1 | 105,695 | 123,072 | 235,214 | 224,013 | 384,800* | 229,129 |
| 512 | 16 | 279,007† | 325,920 | 701,475† | 467,291 | 1,325,332 | 330,525 |

### GET, 1024 byte values

| connections | pipeline | mutr 1×64 | Redis 1 | mutr 2×64 | Redis io-4 | mutr 4×64 | mutr 4×1 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 8 | 1 | 100,399 | 111,495 | 108,707 | 107,981 | 93,890 | 92,055 |
| 8 | 16 | 466,034 | 427,866 | 820,906 | 424,633 | 1,216,937* | 378,987 |
| 32 | 1 | 118,410 | 152,247 | 235,324 | 178,820 | 437,316* | 271,346 |
| 32 | 16 | 469,323 | 352,499 | 879,376 | 557,457 | 1,506,318* | 387,616 |
| 128 | 1 | 122,003 | 162,628 | 239,632 | 241,181 | 454,232* | 209,006 |
| 128 | 16 | 372,652 | 286,442 | 850,306 | 549,847 | 1,399,546* | 355,896 |
| 512 | 1 | 106,830 | 137,290 | 216,152 | 253,145 | 429,076* | 262,769 |
| 512 | 16 | 400,559 | 334,266 | 810,927 | 506,511 | 1,192,054* | 367,930 |

### Mixed 1:1, 1024 byte values

| connections | pipeline | mutr 1×64 | Redis 1 | mutr 2×64 | Redis io-4 | mutr 4×64 | mutr 4×1 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 8 | 1 | 98,176 | 103,254 | 107,681 | 105,895 | 92,725 | 90,962 |
| 8 | 16 | 387,390 | 361,626 | 784,347 | 389,008 | 1,159,115 | 361,804 |
| 32 | 1 | 123,951 | 151,309 | 245,191 | 152,744 | 410,187* | 232,529 |
| 32 | 16 | 343,940 | 354,883 | 823,452 | 542,080 | 1,506,359 | 354,561 |
| 128 | 1 | 123,735 | 157,256 | 239,837 | 179,579 | 463,877* | 253,904 |
| 128 | 16 | 354,074 | 331,858 | 785,560 | 559,707 | 1,364,147 | 339,657 |
| 512 | 1 | 100,151 | 116,005 | 232,057 | 215,461 | 387,423* | 208,750 |
| 512 | 16 | 300,717 | 313,151 | 723,329 | 513,454 | 1,265,995 | 330,917 |

p50, p99, and p99.9 for every replicate are in the CSV. They move with the inverse of the ops/sec above: the 2.5M op/s SET cells have sub-millisecond p50, and the 512-connection pipeline-16 cells sit at several milliseconds because each connection holds a deeper queue.

## A valid row that is still an outlier

mutr 2×64, SET, 1024 byte values, 512 connections, pipeline 16, replicate 4 is `valid=1` at 17,097 ops/s with server CPU 42%. The other three valid replicates of that cell are about 689k, 714k, and 730k ops/s at about 197% server CPU. The median of four values is the average of the two central ones, so the table still shows 701,475. That replicate is in the CSV and in the median.
