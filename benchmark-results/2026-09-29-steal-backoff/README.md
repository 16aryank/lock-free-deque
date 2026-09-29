# Bounded steal backoff comparison

Measured on 2026-09-29 on macOS 15.7.9 ARM64 with Apple Clang 17. The
reference binary was rebuilt from `HEAD:test/benchmark_deque.cpp` at commit
`152e741` with the current, unchanged deque headers. The candidate was built
from the working tree and run in `backoff` mode with the default four attempts
and a jitter window of 4 to 256 pause units. Both builds used
`-std=c++26 -O3 -DNDEBUG`.

Each run processed 3,000,000 items. At each thief count, both variants had two
warm-ups, then ten measured pairs with alternating order. All 100 measured
runs passed exact-once validation. Positive time reduction means backoff was
faster. The mutex column reuses the median from
[`weaker-orders-stage-1/summary.csv`](../weaker-orders-stage-1/summary.csv);
the mutex implementation was not rerun. That historical comparison used the
same workload and machine, but was not paired with these runs.

| Thieves | Previous median (s) | Backoff median (s) | vs previous | Saved mutex median (s) | vs mutex | Faster pairs |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | 0.016377 | 0.016345 | 0.2% | 0.037648 | 56.6% | 3/10 |
| 1 | 0.065597 | 0.062108 | 5.3% | 0.047940 | -29.6% | 6/10 |
| 2 | 0.098138 | 0.028637 | 70.8% | 0.220591 | 87.0% | 10/10 |
| 4 | 0.326277 | 0.030491 | 90.7% | 0.141232 | 78.4% | 10/10 |
| 8 | 0.677145 | 0.123559 | 81.8% | 0.204754 | 39.7% | 10/10 |

The zero-thief difference is noise-sized, and the one-thief pairs overlap.
At two, four, and eight thieves, every paired backoff run was faster. The
historical mutex percentages are useful context, but they may include
cross-session machine variation. See [summary.csv](summary.csv) for medians,
percentage reductions, and ranges; [raw.csv](raw.csv) for all measured runs;
[runs.log](runs.log) for warm-ups and original output; and
[metadata.json](metadata.json) for source and binary hashes.

`make test` and `make test-tsan` passed all 33 tests. A four-thief, 10,000-item
backoff run under ThreadSanitizer passed exact-once validation.

Reproduce the paired comparison with:

```sh
git show 152e741:test/benchmark_deque.cpp > build/benchmark_previous.cpp
g++ -Isrc -std=c++26 -I/opt/homebrew/include -O3 -DNDEBUG build/benchmark_previous.cpp -pthread -o build/benchmark_previous
make build/benchmark_deque
python3 benchmarks/compare_backoff.py build/benchmark_previous build/benchmark_deque benchmark-results/NEW-DIRECTORY
```
