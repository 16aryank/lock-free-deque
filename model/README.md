# Bounded deque model

`stage2_model.c` is a manual C11 translation of the deque and pool protocol.
It is intended for GenMC. It keeps the same ordering, branching, and order of
atomic operations as the C++ source for each selected variant, including
both thief array snapshots, the modulus recheck, growth copies, the
low-water-mark shrink copy, top CAS success and failure, bottom restoration,
and immediate pool return. It uses permanent records with capacities four
and eight. A second deque can claim the returned eight-slot record and push
value 100 while either thief still holds an old pointer.

The bound is four prefilled values (0 through 3), one owner performing three
pops, two thieves performing one steal each, and one borrower attempting one
claim and push. The main thread joins all workers, drains the original deque,
checks that each original value was consumed exactly once, checks that no
original consumer returned the borrower value or an uninitialized slot, and
returns every held record. The C translation uses static arrays and an
`UNINITIALIZED` marker in slots to turn an invalid successful read into an
assertion failure. It has no operation-path allocation or reclamation.

Variants are selected with compiler definitions:

| Definition | Modeled orders |
| --- | --- |
| `MODEL_SC_REFERENCE` | Original all-SC deque and pool |
| none | Stage 1 owner loads and pool handoff |
| `MODEL_STAGE2_CANDIDATE` | Stage 1 plus the stage 2 acquire/release stores, loads, and SC fences; shrink and top CAS remain SC |
| `MODEL_STAGE3_CANDIDATE` | Stage 1 indices and pool, release slot writes (including copies), acquire thief speculative reads; owner copy/pop reads remain SC |

The three `MODEL_PROBE_*` definitions deliberately assert that a particular
path is unreachable. GenMC's expected assertion failure demonstrates that
the path is reachable. They probe shrink CAS success, shrink CAS failure, and
a thief's speculative read of value 100 after another deque borrows the
buffer. The unrestricted variant supplies the safety check for those paths.

## GenMC result

Image: `genmc/genmc@sha256:260f1a6e9c6a746c1ca51ca6dd15b3a57e0ee684f29df0aa000b5372687c408c`
(amd64, run under Docker Desktop's amd64 emulation). The image reports GenMC
v0.19.0, commit `9f6c4c0`, built with LLVM 19.1.7. The modeled language is
C11; `--sc` checks the all-SC reference, and `--rc11` checks the mixed-order
variants. Example command from the repository root:

```sh
docker run --rm --platform linux/amd64 --network none \
  -v "$PWD/model:/work:ro" -w /work \
  genmc/genmc@sha256:260f1a6e9c6a746c1ca51ca6dd15b3a57e0ee684f29df0aa000b5372687c408c \
  genmc --rc11 --disable-estimation --print-error-trace -- \
  -std=c11 -DMODEL_STAGE2_CANDIDATE stage2_model.c
```

| Variant | Memory model | Complete executions | Result |
| --- | --- | ---: | --- |
| `MODEL_SC_REFERENCE` | SC | 4,146 | No errors |
| Stage 1 (no definition) | RC11 | 4,146 | No errors |
| `MODEL_STAGE2_CANDIDATE` | RC11 | 4,146 | No errors |
| `MODEL_STAGE3_CANDIDATE` | RC11 | 4,146 | No errors |

Each `MODEL_PROBE_*` run under the stage 2 candidate produced the expected
assertion failure, confirming that all three paths occur within this bound.
These probe failures are coverage evidence, not algorithm errors. The image
crashed when the C compiler generated an LLVM `freeze` instruction for a
combined conditional return; the equivalent separate conditions in `steal`
avoid that tool failure. `--disable-estimation` skips an optional preliminary
estimate, not the exhaustive check.

The three reachability probes also fired under `MODEL_STAGE3_CANDIDATE`.
The borrower-read trace shows a thief's acquire slot load reading value 100
from the borrower's release slot store; the thief's old top CAS fails.

## Ordering argument for stage 2

The release bottom store follows the task's initialization and SC slot write.
A thief that acquires that bottom value observes the task and any preceding
growth copy. Growth also releases the new array pointer. If a thief's bottom
acquire observes a post-growth bottom write, its later second array snapshot
cannot read a pointer older than that growth publication. The first array
snapshot stays before the bottom read, as in the C++ implementation.

The owner's release bottom decrement reserves a candidate pop before its SC
fence and SC top read/CAS. A thief takes an acquire top snapshot, then an SC
fence before reading bottom and trying its SC top CAS. The fences and CASes
participate in the SC arbitration order across the two indices. Every owner
bottom restoration is release so a thief does not rely on an earlier push's
release sequence through a plain store. The shrink pointer publication,
index shifts, failed-CAS restoration, both thief array snapshots, slot
accesses, and all top CAS outcomes remain SC.

For immediate reuse, a successful shrink top CAS invalidates stale top
claims before the pool release. On shrink CAS failure, the failure's SC read
observes an intervening top update, and the SC bottom restoration precedes
the pool release. A borrower acquires that release before overwriting SC
slots. A stale thief that reads the borrower's SC slot write then reaches its
SC top CAS after the invalidating top update; its expected old top cannot
claim the borrower's value. The bounded check exercises this chain, including
a speculative borrower-value read whose CAS fails. Other stale-pointer
executions still rely on the retained SC snapshots and modulus recheck.

## Ordering argument for stage 3

A push or array copy writes its slot with release ordering. A thief's acquire
speculative read synchronizes with whichever release slot write supplied the
value. If that is a borrower's write, the path from the old owner's shrink
invalidation through pool release/acquire precedes the borrower's write and
the thief's later top CAS. On shrink CAS success, the old owner's SC top
update invalidates the old claim. On CAS failure, its SC failure read observes
an intervening top update before the SC bottom restoration and pool release.
The same release ordering covers `store_no_mark` during growth and shrink
copies. Owner copy and pop reads stay SC in this candidate.

This is a bounded translation, not a proof for arbitrary capacities or
executions. It covers one retained buffer and one borrow/return cycle; it
does not cover multiple retained arrays, repeated ABA pointer reuse,
pointer payload lifetime, or lock-free progress. The native pointer-payload
and concurrent owner/thief tests in `test/test_ordering.cpp` supplement it.
The stage 2 and stage 3 ordering variants in this model are experimental;
the active deque source still uses stage 1 ordering.
