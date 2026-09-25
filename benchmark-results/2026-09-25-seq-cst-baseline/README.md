# Sequential-consistency benchmark baseline

This is the baseline before changing the deque's memory ordering. Each
comparison uses 3,000,000 items with the same number of thieves for the
lock-free and mutex deques. The owner pushes while thieves steal, then drains
remaining items. The optimized benchmark binary was built with the Makefile's
`-O3 -DNDEBUG` flags. Worker setup and validation are outside the timer.

For each thief count, two comparisons warmed up the process, followed by ten
measured comparisons. Each comparison ran the lock-free deque first and the
mutex deque second. Every measured run passed exact-once validation.

| Thieves | Lock-free median (s) | Mutex median (s) | Mutex / lock-free time |
| ---: | ---: | ---: | ---: |
| 0 | 0.015674 | 0.038817 | 2.477× |
| 1 | 0.130083 | 0.051520 | 0.396× |
| 2 | 0.205776 | 0.257809 | 1.253× |
| 4 | 0.508049 | 0.143802 | 0.283× |
| 8 | 0.890590 | 0.206113 | 0.231× |

The ratio is the mutex median time divided by the lock-free median time; a
ratio above 1 means the lock-free deque was faster in this workload. Use
`raw.csv` for every measured time, `runs.log` for original output and
warm-ups, `summary.csv` for machine-readable medians, and `metadata.json`
for the commit, compiler, machine, source hashes, binary hash, and exact
protocol. The four-thief lock-free runs ranged from 0.359478 to 0.536427
seconds, so compare distributions as well as medians.

After changing memory ordering, rerun the identical protocol with a new
output directory:

```sh
python3 benchmarks/record_comparison.py benchmark-results/NEW-DIRECTORY
```

Keep the benchmark source and Makefile unchanged when comparing results.
The benchmark sources matched the recorded commit; the working tree also
contained a README edit and the new collector. The collector records hashes
so the benchmark inputs can be checked directly.
