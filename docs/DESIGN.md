# Design

mutr is one process with a shared in-memory store. The store uses a mutex per shard. It is not lock-free. This file describes the store as it is now. The server in this revision is still one thread; threading is added in a later step and this file should be updated when that lands.

## Shard layout

`--shards N` selects the shard count. `N` must be a power of two from 1 through 1048576. The default is 64. Any other value prints `invalid shards` and exits 2, before the process listens. Rounding a bad value would hide a misconfiguration, so the process rejects it.

Each shard is an `alignas(64)` object that holds its own `std::unordered_map` and its own `std::mutex`. C++ requires `sizeof` to be a multiple of the alignment, so the compiler pads the shard out to a whole number of 64-byte lines. The shards live in one contiguous array. Neighboring mutexes therefore start on different cache lines and do not false-share with each other. The map's buckets are on the heap; only the map's control block sits next to its mutex, which is the same shard.

## Hashing

The key is hashed once, with a 64-bit FNV-1a followed by a splitmix64 finalizer, and the shard index is the high bits:

```text
shard = hash >> (64 - log2(N))
```

`N == 1` is the exception. `log2(1)` is 0, and a 64-bit shift by 64 is undefined. That configuration maps every key to shard 0 without shifting.

FNV-1a alone leaves the high bits poorly mixed on short keys, and the index is those bits, so the finalizer is part of the shard choice. The low bits are not used. `std::unordered_map` picks a bucket from its own hash of the key, which is dominated by the low bits when the bucket count is a power of two. Reusing those bits for the shard index would tie the shard to the bucket.

`std::unordered_map` hashes the key again inside the shard. A single hash cannot be reused for both, because `unordered_map` has no interface that accepts a precomputed hash, and the shard is required to be an `unordered_map`. The hash above is only the shard selector.

## Locks

A single-key command (`GET`, `SET`, `DEL` of one key, `EXISTS` of one key, `INCR`, and lazy expiry) takes that key's shard mutex and holds it for the whole operation. `INCR` reads, checks the integer, and writes the new value before it unlocks, so two `INCR`s on one key cannot lose an update. Lazy expiry is the same critical section: an expired value is erased before the command decides what to return or write. There is no gap where another command can observe the expired value or increment it.

`MSET` and `MGET` need more than one shard. So do variadic `DEL` and `EXISTS`. Those four commands lock every shard they touch, in ascending shard-index order, and hold all of those locks until the command finishes. The same order on every multi-key path is what prevents deadlock. Locking in key order, or locking one shard at a time and then reaching for another, deadlocks when two commands touch the same shards in opposite orders. A single-key command locks one shard, which is already a trivial case of ascending order, so it cannot deadlock with a multi-key command.

`MSET` is atomic across its keys. Another command sees every key from that `MSET`, or none of them. `MGET` is a snapshot of the same kind: the values it returns were all present together under those locks. The other option was to take one shard lock per key and release it before the next key. That cannot deadlock and holds less, and it also lets a reader observe a torn `MSET` (some keys updated, others not). The atomic behavior is the one implemented. The cost is that an `MSET` across many shards holds many mutexes at once, and every other command on those shards waits.

Duplicate keys in one `EXISTS` each count, matching the single-connection behavior from before sharding. The locks are still taken once per shard.

`setNowForTest` writes a virtual clock with no mutex. Tests call it before they start other threads. Calling it concurrently with `get` or `INCR` is a data race on that clock. Production does not call it.

## Known limitations

- The server still accepts one thread. `--threads` other than 1 is rejected.
- There is no active expiry scan. An expired key occupies its shard until the next command touches it.
- `MSET`/`MGET`/`DEL`/`EXISTS` scale their lock hold with the number of distinct shards in the command, not with the number of keys on one shard.
- The second hash inside `unordered_map` is extra work on every lookup. It is a consequence of using `unordered_map`, not a second shard hash.
