# Bounded lock-free work-stealing deque

This is a [Chase–Lev work-stealing deque](chase-lev-paper.pdf). One owner thread pushes and pops at the bottom; any number of thieves may call `steal()` at the top. The implementation uses C++26 in the supplied Makefile.

## Build and check

```sh
make test
make test-tsan
make benchmark
make benchmark BENCH_ARGS="1000000 4"
make benchmark-compare BENCH_ARGS="1000000 4"
make benchmark-tsan TSAN_ARGS="10000 4"
```

GoogleTest is needed for unit tests. The benchmark has no GoogleTest dependency. The supplied Makefile uses Homebrew paths for GoogleTest; adjust `CXXFLAGS` and `LDFLAGS` for another installation. The benchmark accepts an item count and the number of thief threads. `make benchmark` measures the lock-free deque; `make benchmark-compare` runs the same workload once with each deque. Use `0` thieves to measure owner-only push and drain. For example:

```sh
make benchmark BENCH_ARGS="1000000 8"
make benchmark-compare BENCH_ARGS="1000000 8"
make benchmark-profile PROFILE_ARGS="10000000 4"
```

`benchmark-profile` requires Samply. The owner pushes items while thieves steal; after they finish, the owner drains any remaining items. Pool construction and worker setup happen before timing. The benchmark provisions one record per size class for its single lock-free deque and checks that every item was consumed exactly once. Timings include worker completion and per-thread result recording. They are workload measurements, not individual operation latency.

## Configure and use

`BufferPool<T>` preallocates a fixed number of permanent buffers for each configured power-of-two capacity. A size class is given as `{log2_capacity, record_count}`. Construct the pool before starting workers and give each deque a reference to it:

```cpp
#include "work_stealing_deque.h"

BufferPool<int> pool({{2, 2}, {3, 2}, {4, 1}}); // capacities 4, 8, 16
WorkStealingDeque<int> deque(pool, 2);

if (deque.try_push_bottom(42) == PushResult::NO_BUFFER_ACQUIRED) {
    // Keep the task and handle the capacity limit in the caller.
}

auto stolen = deque.steal();
if (stolen.state_ == StealState::SUCCESS) {
    int task = *stolen.value_;
}
```

The constructor throws `std::bad_alloc` if no initial buffer is available.

Multiple deques may share a pool, provided each has its own owner. Provision enough records for buffers held by all active deques and their growth chains. A class may be temporarily unavailable even when another owner is about to return a buffer.

## Buffer lifetime and shutdown

Each pool record and its `std::atomic<T>` slots stay allocated at a fixed address until pool destruction. Growth retains smaller buffers in an owner-only chain. Shrink can return discarded buffers to the pool immediately after its index invalidation sequence. A delayed thief may still hold a raw pointer to one of those buffers: it reads only immutable metadata and atomic slots, and discards the speculative value if its claim CAS fails. Reuse does not free storage.

Shutdown is external and ordered:

1. Stop admitting deque operations; stop and join every participating worker.
2. Ensure no deque or pool operation remains in flight.
3. Destroy all deques, returning their active and retained buffers.
4. Destroy the pool, which then frees records and slot storage.

Keep the deque object itself alive while thieves can use its indices. The pool does *not* manage the lifetime of objects referenced by task pointers. Logical indices must not overflow; physical slot wraparound is supported. The pool rejects size classes beyond its conservative shift limit.

## Lock-free scope and validation

The guaranteed operation paths are `try_push_bottom`, `pop_bottom`, `steal`, and pool claim/return after setup. Pool scans and buffer copies have finite bounds; no path waits for a reader or calls an allocator. A suspended owner may retain buffers but cannot prevent another operation from returning success, empty, abort, or the documented resource result. Finite storage does not promise that every push succeeds. Construction, destruction, worker coordination, and benchmark bookkeeping are outside this guarantee.

The pause hooks used by the schedule tests exist only when compiled with `DEQUE_TEST_HOOKS`; normal and benchmark builds omit them.

`LockFreeAtomicValue<T>` requires trivial copy and move operations and an always-lock-free `std::atomic<T>`. The build also checks lock-free atomic indices, active pointers, and pool flags on the target. All deque, slot, and pool atomic operations currently use sequential consistency. This is a conservative ordering baseline, not a claim that tests prove linearizability or progress.

The unit suite includes deterministic pause schedules for stale thieves, buffer reuse, both shrink CAS outcomes, temporary empty states, and last-item races. It also checks size-class exhaustion, exact item values across growth and shrink, and allocation-free calls after setup. `make test-tsan` and `make benchmark-tsan` check for data races on exercised schedules. However, keep note that passing tests or ThreadSanitizer is not a proof of correctness for every possible schedule.
