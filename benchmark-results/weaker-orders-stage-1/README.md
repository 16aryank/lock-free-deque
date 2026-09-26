# Stage 1 ordering result

The candidate changes only owner loads and pool ownership handoff. Deque
publication, thief snapshots, top arbitration, and slot operations remain
sequentially consistent. The SC reference was built from `HEAD` into
`build/benchmark_deque_reference` with the same Apple Clang and optimized
flags as the candidate.

All 24 unit tests passed both normally and under ThreadSanitizer. The
ThreadSanitizer benchmark passed with 10,000 items and four thieves. Each
benchmark run below checked exact-once consumption.

The standard 3,000,000-item collector ran two warm-ups and ten measured
comparisons per thief count. Its lock-free medians are in `summary.csv`;
`raw.csv`, `runs.log`, and `metadata.json` retain the individual runs and
build details.

`summary.csv` also reports arithmetic average seconds for each implementation
over the ten measured trials, excluding warm-ups. It was regenerated from
`raw.csv` after the collector gained these columns; the recorded timings and
capture metadata are unchanged.

An additional same-machine experiment ran one warm-up per binary and ten
reference/candidate pairs per count, alternating which ran first. Its data
are in `alternating.csv` and `alternating-summary.json`:

| Thieves | Fresh SC median (s) | Stage 1 median (s) | Stage 1 change |
| ---: | ---: | ---: | ---: |
| 0 | 0.015180 | 0.015103 | 0.5% faster |
| 1 | 0.126815 | 0.095271 | 24.9% faster |
| 2 | 0.207863 | 0.103659 | 50.1% faster |
| 4 | 0.460191 | 0.525364 | 14.2% slower |
| 8 | 0.860018 | 0.884150 | 2.8% slower |

The standard collector's four-thief candidate median was 0.470855 s, while
the alternating run measured 0.525364 s. This variation and the four-thief
regression leave the performance acceptance gate open. These measurements
cover the original push/steal workload; they do not establish a benefit for
concurrent owner pops or resize-heavy workloads.
