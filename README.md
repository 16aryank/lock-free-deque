# Lock Free Work-Stealing Deque
An implementation of a Chase-Lev Dynamic Circular Work-Stealing Deque. The implementation is based on [this](https://www.dre.vanderbilt.edu/~schmidt/PDF/work-stealing-dequeue.pdf) paper and is written in C++20.

## Building and benchmarking

```sh
make test                                      # GoogleTest suite
make benchmark                                 # optimized benchmark, 100,000 items
make benchmark BENCH_ARGS="1000000 4 3"         # items, maximum thieves, repetitions
make benchmark-tsan                            # TSAN benchmark, 10,000 items
make benchmark-tsan TSAN_ARGS="100000 4"
make test-tsan                                 # existing tests with TSAN
make benchmark-profile                         # Samply: original deque, streaming, 4 thieves
samply load build/profile.json.gz              # open the recorded profile
```

Implementation headers live in `src/`; unit tests and benchmarks live in `test/`.
Executables and TSAN debug-symbol bundles are generated in `build/`; `make clean`
removes that directory.

The standalone benchmark needs no GoogleTest dependency. It measures owner-only
push/pop pairs, thieves draining a prefilled deque, an owner pushing while thieves
steal, and burst pushes/pops competing with thieves (including resizing). Thief
counts increase from one by powers of two, ending at the requested maximum.
Only one thread accesses the bottom of each deque.

Every workload runs both the original deque and
`mutex_deque::WorkStealingDeque<T>` from `src/mutex/work_stealing_deque.h`.
The baseline protects a plain circular buffer with one `std::mutex`, covering
each complete push, pop, or steal. It doubles when full (leaving one slot unused)
and shrinks on owner pops below quarter capacity, down to its initial size.
Steals wait for the mutex and return success or empty; they do not return abort.
It allocates replacement buffers on resize instead of using the original's shared
buffer pool, so this compares complete implementations, not just synchronization
primitives.

The optional third argument sets repetitions (default 3). Execution order
alternates between implementations. Each `comparison` line reports throughput
from median elapsed times and `speedup = mutex_time / lock_free_time`: values
above 1 mean the original deque is faster; values below 1 mean the mutex baseline
is faster. Speedup is omitted if any repetition fails validation. Both versions
use identical item counts, initial capacities, owner workloads, thief counts,
and validation, although scheduling affects the division of work and retry counts.

Optional fourth and fifth benchmark arguments select an implementation
(`both`, `lock-free`, or `mutex`) and workload (`all`, `owner`, `prefilled`,
`streaming`, or `mixed`). Defaults preserve the full comparison suite. Selecting
one workload uses exactly the requested thief count (zero for `owner`). For example:

```sh
make benchmark BENCH_ARGS="1000000 4 3 lock-free streaming"
make benchmark-profile PROFILE_ARGS="10000000 4 5 lock-free prefilled" PROFILE_OUTPUT=build/prefilled.json.gz
```

The profiling target requires Samply and builds a separate optimized binary with
debug symbols and frame pointers. It saves the profile and a symbol sidecar in
`build/`. Profiling includes setup and validation outside the timed workload;
inspect worker stacks to isolate stealing. On macOS, the original deque's atomic
shared-pointer loads use libc++ internal mutexes, so the `lock-free` benchmark
label does not imply that the complete implementation is lock-free.

Output includes consumed items/second, deque calls/second (including failed
attempts), elapsed nanoseconds/item, owner/stolen counts, and abort/empty counts.
Prefilling, thread creation, and exact-once validation are excluded from timing;
per-thread result recording, retries, and worker completion are included. Each
scenario checks for missing, duplicate, and out-of-range items after timing and
returns a failing exit status on errors or a 30-second cooperative timeout.
These are whole-workload throughput measurements, not individual call latency.
Run several times on an otherwise idle machine to assess timing variability.

Optimized (`-O3`) and TSAN (`-O1 -g -fsanitize=thread`) binaries are separate;
use optimized results for speed comparisons. TSAN targets stop on the first race. A passing item check or TSAN run does not prove correctness of the whole algorithm.

## Implementation overview

The general algorithm is described in the above paper. The deque follows the paper exactly. I implemented this for the following reasons:

1. Learn about atomicity, specifically the `std::atomic` library, and learn about lock-free data structures.
2. Learn how to implement data structures / algorithms based on academic papers, since I've never done that before. In the process of implementing the deque, I also read a paper on the ABA problem and learned a few ways how to implement them.
3. Learn more about C++ features. Beyond `std::atomic`, I learned about `std::enable_shared_from_this`, `std::hazard_pointer` (although I did not use them since my toolchain does not yet support C++26), and the `explicit` keyword.

Below the underlying data structures for the deque are described. 

## Circular Array

The implementation of the array is stored in `src/circular_array.h`. The array has five private data members: the log of its size, `log_size_`; a unique pointer to the underlying array of variables, `segment_`; a low water mark, `low_water_mark_`; a shared pointer to the previous circular array, `prev_`; and an atomic pointer to the next pool, `pool_next_`.

`segment_` owns a `std::atomic<T>[]` array. All slot reads and writes, including
growth and shrink copies, use relaxed atomic operations; deque publication and
ownership ordering are handled separately by the indices. Both `CircularArray<T>`
and `WorkStealingDeque<T>` require `LockFreeAtomicValue<T>`: an unqualified,
trivially copyable type satisfying the copy/move requirements of `std::atomic<T>`,
with `std::atomic<T>::is_always_lock_free` true on the target platform. Integers
and task pointers are typical choices; large structs and nontrivial objects are
rejected. As before, allocating the array also requires default construction of
the value type. Lock-free slots alone do not guarantee that every deque operation
is lock-free.

The low-water mark is used as an optimization while shrinking. The previous, smaller array will still contain a lot of the same valid data, so you will only need to copy over the delta of elements that were added while the bigger array was active. As Chase and Lev describe:

> When a deque shrinks its array, only the elements stored in indexes greater than or equal to the low water mark of the bigger array are copied

To store the previous array, the Circular Array class publically inherits from `std::enable_shared_from_this`. Without it, the getting the previous array via a new shared pointer would create two independent control blocks for the same object, leading to double deletion and null pointer accesses. Instead, the `std::shared_from_this` allows the array to safely generate additional shared pointer instances of the same ownership.

The array uses a shared pool to store the arrays after they’re no longer reachable by any deque or in‑flight stealer. This happens when the last shared_ptr reference to that array goes away. The pool reuses arrays via the custom deleter, so the arrays are still eventually recycled. The pool is implemented via a vector of unique pointer of Trieber Stacks of the Circular Array, where the index of the vector represents the log size of the Circular Array. 

## Treiber Stack

Treiber Stack's work by appending an element to the top of the stack _iff_ that element is guaranteed, via a CAS (weak) loop, to be the only element to be the added since the operation began. Since C++ does not have garbage collection, tagged pointers are used to sovle the ABA problem. 
