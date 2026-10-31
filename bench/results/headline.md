# Headline

Paired ratio = same-replicate ops/sec. Ratios half-up to 2 decimals. Ops/sec and latency unrounded.

## 64 B, SET, P16, 128 connections

mutr 4x64 / Redis io-4: 2.17 / 2.48 / 2.97
mutr 4x64 / Redis 1: 2.51 / 2.89 / 3.97

mutr 4x64: median ops/sec 2510616.23, median p50 0.735 ms, median p99 2.023 ms, median p99.9 3.615 ms, client_bound no, invalid no
Redis io-4: median ops/sec 845437.91, median p50 2.063 ms, median p99 3.231 ms, median p99.9 5.023 ms, client_bound no, invalid no
Redis 1: median ops/sec 978863.00, median p50 2.239 ms, median p99 3.679 ms, median p99.9 7.231 ms, client_bound no, invalid no

## 64 B, GET, P16, 32 connections

mutr 4x64 / Redis io-4: 2.25 / 2.57 / 2.64
mutr 4x64 / Redis 1: 2.36 / 4.31 / 4.76

mutr 4x64: median ops/sec 2209674.11, median p50 0.207 ms, median p99 0.495 ms, median p99.9 1.383 ms, client_bound no, invalid no
Redis io-4: median ops/sec 839718.62, median p50 0.599 ms, median p99 0.783 ms, median p99.9 1.175 ms, client_bound no, invalid no
Redis 1: median ops/sec 478007.50, median p50 1.079 ms, median p99 1.279 ms, median p99.9 1.831 ms, client_bound no, invalid no

## 1024 B, SET, P16, 128 connections

mutr 4x64 / Redis io-4: 2.51 / 2.73 / 4.18
mutr 4x64 / Redis 1: 2.75 / 3.19 / 4.95

mutr 4x64: median ops/sec 1354689.41, median p50 1.343 ms, median p99 3.263 ms, median p99.9 5.887 ms, client_bound no, invalid no
Redis io-4: median ops/sec 401180.37, median p50 4.703 ms, median p99 10.111 ms, median p99.9 34.047 ms, client_bound no, invalid no
Redis 1: median ops/sec 288599.16, median p50 6.527 ms, median p99 14.527 ms, median p99.9 19.455 ms, client_bound no, invalid no

## mutr 4x1 / mutr 4x64, 64 B, P16

SET, 32 connections: 0.21 / 0.23 / 0.44
SET, 128 connections: 0.19 / 0.22 / 0.25
SET, 512 connections: 0.22 / 0.23 / 0.24
GET, 32 connections: 0.23 / 0.23 / 0.28
GET, 128 connections: 0.23 / 0.24 / 0.25
GET, 512 connections: 0.14 / 0.23 / 0.28

## mutr 4x64 / Redis io-4, all valid non-client-bound cells

28 cells with all 5 replicates of both servers valid and not client_bound. 20 cells have a client_bound replicate. Invalid replicates on either server: 0. Value per cell = median of the 5 paired ratios.

min / median / max: 0.78 / 2.47 / 2.94
