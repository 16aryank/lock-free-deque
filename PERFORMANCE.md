# Performance: from the baseline to steal backoff

The chart compares the mutex deque, the original sequentially consistent
lock-free deque, the lock-free deque after cache-line separation, and the
latest lock-free deque with bounded steal backoff. Each bar is a median runtime
for processing 3,000,000 items; lower is faster. Panels use separate time
scales to keep the small bars readable.

![Bar chart of median runtimes for four deque implementations](benchmark-results/implementation-comparison.svg)

| Thieves | Mutex (s) | Sequential consistency (s) | Cache layout (s) | Steal backoff (s) |
| ---: | ---: | ---: | ---: | ---: |
| 0 | 0.037648 | 0.015674 | 0.015835 | 0.016345 |
| 1 | 0.047940 | 0.130083 | 0.070263 | 0.062108 |
| 2 | 0.220591 | 0.205776 | 0.094790 | 0.028637 |
| 4 | 0.141232 | 0.508049 | 0.318180 | 0.030491 |
| 8 | 0.204754 | 0.890590 | 0.687449 | 0.123559 |

Steal backoff has the lowest saved median at two, four, and eight thieves.
The mutex deque has the lowest median at one thief. With no thieves, the three
lock-free variants are close together; the mutex deque takes roughly twice as
long. The small differences among the lock-free variants at zero thieves
should not be read as a meaningful ranking.

## How the implementation evolved

**Sequential consistency established the baseline.** The original lock-free
deque used sequentially consistent atomic operations throughout. The
[baseline comparison](benchmark-results/2026-09-25-seq-cst-baseline/README.md)
also measured the mutex deque. At four and eight thieves, the SC lock-free
median was 0.508 and 0.891 seconds, compared with mutex medians of 0.144 and
0.206 seconds in that same baseline. This identified the multi-thief workload
as the main performance problem, without identifying its cause.

**Stage 1 changed memory ordering.** Owner-only loads became relaxed, while
pool record return and claim became release and acquire operations. Deque
publication, thief snapshots, top arbitration, and slot accesses stayed
sequentially consistent. In a same-machine alternating comparison, this was
24.9% faster with one thief and 50.1% faster with two, but 14.2% slower with
four and 2.8% slower with eight. The four-thief timing also varied between
protocols, so the [Stage 1 report](benchmark-results/weaker-orders-stage-1/README.md)
did not establish a broad performance gain.

**Cache-line separation reduced false sharing.** The next change placed the
active array pointer, owner bottom index, thief top index, and cached top on
separate cache lines. On this Apple ARM64 host the deque grew from 48 to 512
bytes. In the [paired cache-layout experiment](benchmark-results/2026-09-26-cache-layout-p1/README.md),
the padded version reduced median time by 24.7%, 37.8%, 37.7%, and 22.2% at
one, two, four, and eight thieves. Its zero-thief result changed by only
-0.7%. The larger object is a space cost for this layout.

**Profiling pointed to contention among thieves.** After the layout change,
the [September 28 profile review](benchmark-results/2026-09-28-profile-review/README.md)
still placed about 70% of four-thief and 79% of eight-thief sampled thief CPU
delta at the instruction following the top compare/exchange. A separate
instrumented diagnostic found median `ABORT` results per success of 2.27 with
four thieves and 4.73 with eight. Those counts include all raw `ABORT` paths,
and the samples do not measure exact CAS latency. Together they motivated a
bounded retry policy around `steal()` rather than another layout change.

**Worker-local backoff reduced multi-thief time.** Each worker now owns its
retry context. The default policy makes at most four fresh steal attempts per
call; after an actual `ABORT`, it grows a jitter window from 4 pause units up
to 256. A success or genuine empty result resets the window. The raw deque
algorithm and its memory orders are unchanged. In the [paired backoff run](benchmark-results/2026-09-29-steal-backoff/README.md),
median time fell by 70.8%, 90.7%, and 81.8% against the no-backoff reference
at two, four, and eight thieves. Backoff was faster in all ten pairs at each
of those counts. At zero and one thief the median changes were only 0.2% and
5.3%, with 3/10 and 6/10 faster pairs; the data do not establish a reliable
benefit there. All 100 measured runs passed exact-once validation.

The backoff tests and ThreadSanitizer checks passed. A separate
[bounded GenMC model](model/README.md) explored 23,764 RC11 executions of
Stage 1 ordering with steal retries without errors. Its smaller retry bound
checks safety schedules, not the runtime or fairness of the pause policy.

## Measurement and comparison limits

The benchmark has one owner pushing items while thieves steal. After the
thieves join, the owner drains any remainder. Worker setup and exact-once
validation are outside the timed interval; worker completion and result
recording are inside it. The saved runs used Apple Clang 17 on the same macOS
ARM64 host and optimized `-std=c++26 -O3 -DNDEBUG` builds. Each source below
reports ten measured trials per thief count, excluding warm-ups:

| Chart series | Saved median source |
| --- | --- |
| Mutex | [Stage 1 comparison](benchmark-results/weaker-orders-stage-1/summary.csv), `mutex_median_seconds` |
| Sequential consistency | [SC baseline](benchmark-results/2026-09-25-seq-cst-baseline/summary.csv), `lock_free_median_seconds` |
| Cache layout | [P1 cache-layout candidate](benchmark-results/2026-09-26-cache-layout-p1/summary.csv), `candidate_median_seconds` |
| Steal backoff | [Backoff candidate](benchmark-results/2026-09-29-steal-backoff/summary.csv), `candidate_median_seconds` |

The four chart columns come from separate recording sessions. In particular,
the mutex implementation was not rebenchmarked for the backoff experiment.
The four-way chart shows the saved results at a common workload, but small
cross-session gaps are not controlled speedups. The paired cache-layout and
backoff experiments support conclusions about those individual changes more
directly. These results do not measure latency, energy use, fairness, other
architectures, many-deque schedulers, or concurrent owner pops.

The chart is also available as a [PNG](benchmark-results/implementation-comparison.png).
Regenerate both files from the saved CSVs with
[`benchmarks/plot_implementation_comparison.py`](benchmarks/plot_implementation_comparison.py):

```sh
python3 -m pip install --target build/plot_deps matplotlib
PYTHONPATH=build/plot_deps MPLCONFIGDIR=build/mpl_config XDG_CACHE_HOME=build/font_cache python3 benchmarks/plot_implementation_comparison.py
```
