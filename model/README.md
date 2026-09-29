# Bounded deque and steal retry model

[`deque_model.c`](deque_model.c) is a manual C11 translation of the active
deque and pool protocol, with the worker-owned steal retry wrapper. It uses
the production Stage 1 memory ordering: owner-only loads are relaxed, pool
claim and return use acquire and release, and deque indices, array pointer
publication, slots, and top arbitration remain sequentially consistent.
There are no compile-time ordering variants or reachability probes.

The bound is four prefilled values (0 through 3), one owner performing three
pops, two thieves each running up to two retry batches, and one borrower
attempting one pool claim and push. Each batch makes at most two fresh raw
steal attempts and stops on `SUCCESS` or `EMPTY`. After an exhausted `ABORT`,
the thief calls the wrapper once more with its retained window. The
production default is four attempts per batch, while this smaller bound
limits the state space. Random jitter and CPU pause are omitted because they
access no shared memory and only change scheduling time. The model checks
retry counts and final window states against the wrapper contract.

The model retains both thief array snapshots, the modulus recheck, growth
copies, low-water-mark shrink copy, top CAS success and failure, bottom
restoration, and immediate pool return. A second deque can claim the
returned eight-slot record and push value 100 while a thief still holds an
old pointer. The main thread joins all workers, drains the original deque,
checks exact-once consumption of each original value, rejects borrower and
uninitialized values as original results, and returns every held record.

Run GenMC with the RC11 memory model from the repository root:

```sh
docker run --rm --platform linux/amd64 --network none \
  -v "$PWD/model:/work:ro" -w /work \
  genmc/genmc@sha256:260f1a6e9c6a746c1ca51ca6dd15b3a57e0ee684f29df0aa000b5372687c408c \
  genmc --rc11 --disable-estimation --print-error-trace -- \
  -std=c11 deque_model.c
```

With the pinned GenMC v0.19.0 image above, the RC11 check completed 23,764
executions with no errors. The native C model also compiled with
`-std=c11 -Wall -Wextra -Werror` and passed its assertions.

This is a bounded safety check, not a proof for arbitrary capacities,
attempt counts, or executions. It does not model more than two batches, victim
switches, multiple retained arrays, repeated ABA pointer reuse, pointer
payload lifetime, timing, or lock-free progress. The native tests for the
retry state machine and the deque's concurrent behavior supplement it.
