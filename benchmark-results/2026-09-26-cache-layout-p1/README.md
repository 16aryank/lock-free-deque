# P1 cache-line separation

Measured on 2026-09-26 on macOS 15.7.9 ARM64, Apple Clang 17. The reference
binary was built from commit `6428dd1` before the layout edit; the candidate
uses the same benchmark source, compiler, and `-std=c++26 -O3 -DNDEBUG` flags.
The only deque change is field alignment. Atomic orders and algorithms are
unchanged. The exact [source diff](layout-change.diff) and binary/source
hashes in [metadata.json](metadata.json) preserve the candidate.

The candidate uses `std::hardware_destructive_interference_size` as its
portable base. This toolchain reports 64 bytes, while `hw.cachelinesize` on
this Apple ARM64 host reports 128 bytes, so the Apple ARM64 policy uses a
128-byte minimum. The [reference layout](reference-layout.txt) places active
array, bottom, top, and cached top at offsets 8, 16, 24, and 32 in a 48-byte
object. The [candidate layout](candidate-layout.txt) places them at 8, 128,
256, and 384 in a 512-byte object aligned to 128 bytes. Arrays of deques
therefore use 512 bytes per deque rather than 48, excluding pool storage.

## Alternating comparison

Each binary processed 3,000,000 items. For each thief count, each binary had
two warm-ups, then ten measured pairs alternated execution order. Every
measured run passed exact-once validation. Positive time reduction means the
candidate was faster.

| Thieves | Reference median (s) | Candidate median (s) | Time reduction | Faster pairs |
| ---: | ---: | ---: | ---: | ---: |
| 0 | 0.015726 | 0.015835 | -0.7% | 7/10 |
| 1 | 0.093255 | 0.070263 | 24.7% | 10/10 |
| 2 | 0.152305 | 0.094790 | 37.8% | 9/10 |
| 4 | 0.510886 | 0.318180 | 37.7% | 10/10 |
| 8 | 0.883937 | 0.687449 | 22.2% | 9/10 |

The zero-thief difference is small and reverses in most paired runs. The
multi-thief gain persists in nine or ten of ten pairs at each count. See
[raw.csv](raw.csv) for every measured run, [runs.log](runs.log) for warm-ups
and original output, and [summary.csv](summary.csv) for medians and ranges.
The result measures this streaming workload, not the cost of many deque
objects in one scheduler.

## Correctness and profiles

`make test` and `make test-tsan` each passed all 26 tests, including an
offset and alignment test. `make benchmark-tsan TSAN_ARGS='10000 4'` also
passed. Before the edit, all 25 existing tests passed both normally and
under ThreadSanitizer.

[Four-thief](candidate-4.json.gz) and [eight-thief](candidate-8.json.gz)
Samply profiles used 10,000,000 items and the candidate profiling build;
both passed exact-once validation. They recorded 0.826593 and 1.932023
benchmark seconds respectively. In the [CPU summary](cpu-summary.json), the
instruction immediately after top's `casal` still accounts for 64.3% and
74.6% of sampled thief CPU delta. Separating owner fields does not remove
contention among thieves. These profile timings include profiler overhead
and are not used for the speedup table.

The profiles used Samply 0.13.1 with
`samply record --save-only --unstable-presymbolicate` and
`./build/benchmark_deque_profile 10000000 THIEVES`. The profiling binary
SHA-256 was
`8769d7869a118af341a69206ef1562d303e60e0d923c491a2e4d17d816896e67`.

Reproduce the unprofiled comparison with:

```sh
python3 benchmarks/compare_variants.py build/p1-layout/reference build/benchmark_deque benchmark-results/NEW-DIRECTORY
```

The reference binary and temporary layout probes are in ignored `build/`.
Rebuild the reference from commit `6428dd1` with
`g++ -Isrc -std=c++26 -I/opt/homebrew/include -O3 -DNDEBUG test/benchmark_deque.cpp -pthread`
in an isolated checkout before repeating this comparison elsewhere.
