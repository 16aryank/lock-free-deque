# Samply rerun after cache-line separation

Recorded 2026-09-28 at commit `152e741c3f0035ce72dd7f9b77e4738359de8789`.
The active implementation includes stage 1 memory ordering and P1 padding.
Apple Clang 17, ARM64 macOS 15.7.9, Samply 0.13.1, 1,000 Hz sampling.
See [metadata](metadata.json) for commands, compiler flags, and hashes.

Production sources were unchanged. The missing `MEMORY_ORDERING_PLAN.md`
was reconstructed from the previous Codex diff and revised to mark P1 complete
and prioritize retries. The prior ordering research and correctness constraints
were retained. This report is a new measurement of the landed implementation;
the [September 26 profiles](../2026-09-26-samply/README.md) remain historical.

## Unmodified benchmark profiles

Each capture processed 10,000,000 items. All passed exact-once validation,
with `owner=0` and `stolen=10000000`. Captures ran sequentially, in the order
four thieves, eight thieves, one thief, four-thief repeat.

| Profile | Benchmark seconds | Thief CPU delta at post-CAS PC | At result-vector store PC |
| --- | ---: | ---: | ---: |
| [One thief](current-1.json.gz) | 0.211861 | 3.76% | 79.24% |
| [Four thieves](current-4.json.gz) | 0.908951 | 70.45% | 0.90% |
| [Four thieves, repeat](current-4-repeat.json.gz) | 0.831469 | 67.47% | 1.69% |
| [Eight thieves](current-8.json.gz) | 1.913733 | 78.59% | 0.17% |

The post-CAS PC is RVA `0x3148`, a comparison following `casal` at `0x3144`.
The result store is RVA `0x30dc`. See [hot instructions](hot-instructions.txt)
and the full [CPU summary](cpu-summary.json). A retained object and dSYM
allow source mapping; `dwarfdump --lookup 0x100003148` places this PC inside
the inlined atomic compare/exchange implementation.

Percentages weight leaf samples by `threadCPUDelta`, separately for the owner
and all thieves. The denominator includes the whole captured process, including
setup and validation. These are sampled instruction neighborhoods, not exact
instruction latencies, hardware cache-miss counts, or CAS failure rates.
The wall times include profiling perturbation and are not a speedup study.

## Interpretation

- **Remaining contention is among thieves.** Top is already isolated from
  the owner fields, but the post-CAS hotspot persists across two four-thief
  captures and grows to about 79% at eight thieves. Further padding cannot
  separate different threads' updates to the same top index.
- **The old owner-store hotspot has changed.** About 52–58% of owner CPU delta
  in the four-thief captures lands in the push-loop call/iteration neighborhood
  in `main`; the old dominant post-bottom-store PC is no longer dominant.
  Do not attribute those samples to a precisely measured function-call cost
  or immediately force inlining: the consumer CAS path is the stronger lead.
- **Result recording matters at one thief.** The worker writes each consumed
  value to a vector. At one thief that store PC dominates sampled CPU delta.
  At multiple thieves it is a small share. A separate benchmark variant can
  isolate recording overhead without weakening exact-once checks.
- **Shrink is still not represented by successful concurrent pops.** The
  benchmark joins thieves before draining, and all items here were stolen.
  These captures do not justify weakening shrink's protocol. The modulus
  check is already a shift/bit test, not a division hotspot.
- **Scheduler yield is not the observed eight-thief hotspot this time.**
  99.95% of eight-thief CPU delta is attributed to the worker wrapper, with
  hot PCs inside the steal path. The earlier capture's `swtch_pri` percentage
  must not be reused as a current measurement.

## Supplemental retry diagnostic

An isolated copy of the benchmark adds a worker-local `AttemptCounts` object.
Each worker accumulates counts locally and copies them to a separate aligned
record after exiting its loop; output occurs after timing. There are no shared
atomic measurement counters or changes to deque memory orders. The exact
change is in [diagnostic.diff](diagnostic.diff); the build used
`-Isrc -std=c++26 -I/opt/homebrew/include -O3 -DNDEBUG`.

Three runs per count processed 3,000,000 items each. All nine passed exact-once
validation with all items stolen. These are instrumented observations, not
unmodified timing comparisons; counters can change scheduling and codegen.

| Thieves | Median aborts/success | Minimum–maximum | Empty returns/run |
| ---: | ---: | ---: | ---: |
| 1 | 0.0000 | 0.0000–0.0000 | 1–2 |
| 4 | 2.2687 | 2.1105–2.7539 | 4–5 |
| 8 | 4.7292 | 3.4410–4.7486 | 9–22 |

See [per-thread counts](diagnostic.json) and [run output](diagnostic.log).
`ABORT` counts API results, including possible modulus-validation aborts;
they do not isolate CAS failures. The high retry count and rare empty returns
support testing bounded abort backoff before changing empty-queue yielding.
Measure whether lower retry counts actually improve elapsed time, fairness,
CPU usage, and latency in a separate paired experiment.

## Reproduce

The profiling build retains its object so debug information remains usable:

```sh
mkdir -p build/profile-20260928
g++ -Isrc -std=c++26 -I/opt/homebrew/include -O3 -g -DNDEBUG -fno-omit-frame-pointer -c test/benchmark_deque.cpp -o build/profile-20260928/benchmark.o
g++ build/profile-20260928/benchmark.o -pthread -o build/profile-20260928/benchmark
xcrun dsymutil build/profile-20260928/benchmark
samply record --save-only --unstable-presymbolicate -o build/profile-20260928/new-4.json.gz -- ./build/profile-20260928/benchmark 10000000 4
samply load benchmark-results/2026-09-28-profile-review/current-4.json.gz
python3 benchmark-results/2026-09-28-profile-review/summarize.py
```

Use distinct outputs for one/eight thieves and repeats. Profiling initially
failed inside the sandbox with error 1100 and succeeded with permission to
sample outside it. All four profiles have symbol sidecars. The checked-in
CPU summary is reproduced by the script above; its algorithm matches the
prior report.

To reproduce the diagnostic, copy the benchmark to an isolated file, apply
the saved diff, compile with the stated flags against the current `src`, and
run `BINARY 3000000 THIEVES` three times for each of 1, 4, and 8 thieves.
The temporary executable and source remain in `build/profile-20260928/`.
No production optimization, contract implementation, or Makefile edit was
made as part of this rerun.
