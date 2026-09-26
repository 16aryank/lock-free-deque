// Bounded C11 translation of the deque/pool protocol for GenMC.
// See README.md for bounds and differences from the C++ implementation.
#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>

#define K 4
#define UNINITIALIZED (-99)
#define EMPTY (-1)
#define ABORT (-2)
#define NO_BUFFER (-3)

#if (defined(MODEL_SC_REFERENCE) && defined(MODEL_STAGE2_CANDIDATE)) || \
    (defined(MODEL_SC_REFERENCE) && defined(MODEL_STAGE3_CANDIDATE)) || \
    (defined(MODEL_STAGE2_CANDIDATE) && defined(MODEL_STAGE3_CANDIDATE))
#error Select one ordering variant
#endif

#ifdef MODEL_SC_REFERENCE
#define OWNER_LOAD_ORDER memory_order_seq_cst
#define POOL_CLAIM_ORDER memory_order_seq_cst
#define POOL_FAILURE_ORDER memory_order_seq_cst
#define POOL_RETURN_ORDER memory_order_seq_cst
#else
#define OWNER_LOAD_ORDER memory_order_relaxed
#define POOL_CLAIM_ORDER memory_order_acquire
#define POOL_FAILURE_ORDER memory_order_relaxed
#define POOL_RETURN_ORDER memory_order_release
#endif

#ifdef MODEL_STAGE2_CANDIDATE
#define PUSH_TOP_ORDER memory_order_acquire
#define GROWTH_POINTER_ORDER memory_order_release
#define PUSH_BOTTOM_ORDER memory_order_release
#define STEAL_TOP_ORDER memory_order_acquire
#define STEAL_BOTTOM_ORDER memory_order_acquire
#define POP_RESERVATION_ORDER memory_order_release
#define POP_RESTORE_ORDER memory_order_release
#define STEAL_FENCE() atomic_thread_fence(memory_order_seq_cst)
#define POP_FENCE() atomic_thread_fence(memory_order_seq_cst)
#else
#define PUSH_TOP_ORDER memory_order_seq_cst
#define GROWTH_POINTER_ORDER memory_order_seq_cst
#define PUSH_BOTTOM_ORDER memory_order_seq_cst
#define STEAL_TOP_ORDER memory_order_seq_cst
#define STEAL_BOTTOM_ORDER memory_order_seq_cst
#define POP_RESERVATION_ORDER memory_order_seq_cst
#define POP_RESTORE_ORDER memory_order_seq_cst
#define STEAL_FENCE() ((void)0)
#define POP_FENCE() ((void)0)
#endif

#ifdef MODEL_STAGE3_CANDIDATE
#define SLOT_STORE_ORDER memory_order_release
#define SPECULATIVE_SLOT_LOAD_ORDER memory_order_acquire
#else
#define SLOT_STORE_ORDER memory_order_seq_cst
#define SPECULATIVE_SLOT_LOAD_ORDER memory_order_seq_cst
#endif

typedef struct Array Array;
struct Array {
    const int log_size;
    _Atomic int slots[8];
    int64_t low_water_mark;
    Array *prev;
    _Atomic int owned;
};

typedef struct {
    _Atomic(Array *) active;
    _Atomic int64_t bottom;
    _Atomic int64_t top;
    int64_t cached_top;  // owner only
    int min_log_size;
} Deque;

static Array small = {.log_size = 2};
static Array large = {.log_size = 3};
static Deque original;
static Deque borrower;
static int owner_results[3];
static int thief_results[2];
static int speculative_values[2] = {UNINITIALIZED, UNINITIALIZED};
static int borrower_acquired;
static int shrink_succeeded;
static int shrink_failed;

static int64_t capacity(const Array *array) {
    return INT64_C(1) << array->log_size;
}

static int slot_load(const Array *array, int64_t i) {
    return atomic_load_explicit(&array->slots[i & (capacity(array) - 1)],
                                memory_order_seq_cst);
}

static int slot_load_speculative(const Array *array, int64_t i) {
    return atomic_load_explicit(&array->slots[i & (capacity(array) - 1)],
                                SPECULATIVE_SLOT_LOAD_ORDER);
}

static void slot_store_no_mark(Array *array, int64_t i, int value) {
    atomic_store_explicit(&array->slots[i & (capacity(array) - 1)], value,
                          SLOT_STORE_ORDER);
}

static void slot_store(Array *array, int64_t i, int value) {
    slot_store_no_mark(array, i, value);
    if (i < array->low_water_mark) array->low_water_mark = i;
}

static Array *try_acquire(int log_size) {
    Array *record = log_size == 3 ? &large : NULL;
    if (!record) return NULL;
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&record->owned, &expected, 1,
                                                  POOL_CLAIM_ORDER,
                                                  POOL_FAILURE_ORDER)) {
        return NULL;
    }
    record->prev = NULL;
    record->low_water_mark = INT64_MAX;
    return record;
}

static void release_array(Array *array) {
    array->prev = NULL;
    atomic_store_explicit(&array->owned, 0, POOL_RETURN_ORDER);
}

static Array *grow_into(Array *source, Array *destination,
                        int64_t bottom, int64_t top) {
    assert(destination && destination != source);
    assert(destination->log_size == source->log_size + 1);
    destination->prev = source;
    for (int64_t i = top; i < bottom; ++i)
        slot_store_no_mark(destination, i, slot_load(source, i));
    return destination;
}

static Array *shrink_into(Array *source, int64_t bottom, int64_t top,
                          size_t num_shrink) {
    int64_t min_low_water = source->low_water_mark;
    Array *destination = source;
    for (size_t i = 0; i < num_shrink; ++i) {
        destination = destination->prev;
        assert(destination);
        if (destination->low_water_mark < min_low_water)
            min_low_water = destination->low_water_mark;
    }
    if (min_low_water < destination->low_water_mark)
        destination->low_water_mark = min_low_water;
    int64_t start = bottom;
    if (min_low_water != INT64_MAX)
        start = top > min_low_water ? top : min_low_water;
    for (int64_t i = start; i < bottom; ++i)
        slot_store_no_mark(destination, i, slot_load(source, i));
    return destination;
}

static int cas_top(Deque *deque, int64_t expected, int64_t desired) {
    return atomic_compare_exchange_strong_explicit(&deque->top, &expected,
                                                    desired,
                                                    memory_order_seq_cst,
                                                    memory_order_seq_cst);
}

static int push_bottom(Deque *deque, int value) {
    int64_t b = atomic_load_explicit(&deque->bottom, OWNER_LOAD_ORDER);
    Array *array = atomic_load_explicit(&deque->active, OWNER_LOAD_ORDER);
    if (b - deque->cached_top >= capacity(array) - 1) {
        int64_t t = atomic_load_explicit(&deque->top, PUSH_TOP_ORDER);
        deque->cached_top = t;
        if (b - t >= capacity(array) - 1) {
            Array *destination = try_acquire(array->log_size + 1);
            if (!destination) return NO_BUFFER;
            array = grow_into(array, destination, b, t);
            atomic_store_explicit(&deque->active, array, GROWTH_POINTER_ORDER);
        }
    }
    slot_store(array, b, value);
    atomic_store_explicit(&deque->bottom, b + 1, PUSH_BOTTOM_ORDER);
    return 0;
}

static int steal(Deque *deque, int *speculative_value) {
    int64_t t = atomic_load_explicit(&deque->top, STEAL_TOP_ORDER);
    STEAL_FENCE();
    Array *old_array = atomic_load_explicit(&deque->active,
                                             memory_order_seq_cst);
    int64_t b = atomic_load_explicit(&deque->bottom, STEAL_BOTTOM_ORDER);
    Array *array = atomic_load_explicit(&deque->active,
                                         memory_order_seq_cst);
    int64_t size = b - t;
    if (size <= 0) return EMPTY;
    if (size % capacity(array) == 0) {
        int64_t top_snapshot = atomic_load_explicit(&deque->top,
                                                     memory_order_seq_cst);
        if (array != old_array) return ABORT;
        if (t != top_snapshot) return ABORT;
        return EMPTY;
    }
    int value = slot_load_speculative(array, t);
    *speculative_value = value;
    return cas_top(deque, t, t + 1) ? value : ABORT;
}

static void perhaps_shrink(Deque *deque, int64_t b, int64_t t) {
    Array *array = atomic_load_explicit(&deque->active, OWNER_LOAD_ORDER);
    Array *cursor = array;
    size_t num_shrink = 0;
    while (cursor->log_size > deque->min_log_size &&
           b - t < capacity(cursor) / K) {
        Array *prev = cursor->prev;
        if (!prev) break;
        cursor = prev;
        ++num_shrink;
    }
    if (!num_shrink) return;

    Array *new_array = shrink_into(array, b, t, num_shrink);
    atomic_store_explicit(&deque->active, new_array, memory_order_seq_cst);
    int64_t shift = capacity(new_array);
    atomic_store_explicit(&deque->bottom, b + shift, memory_order_seq_cst);
    int64_t top_snapshot = atomic_load_explicit(&deque->top,
                                                 memory_order_seq_cst);
    if (!cas_top(deque, top_snapshot, top_snapshot + shift)) {
        ++shrink_failed;
        atomic_store_explicit(&deque->bottom, b, memory_order_seq_cst);
    } else {
        ++shrink_succeeded;
    }

    Array *discarded = array;
    while (discarded != new_array) {
        Array *next = discarded->prev;
        release_array(discarded);
        discarded = next;
    }
}

static int pop_bottom(Deque *deque) {
    int64_t b = atomic_load_explicit(&deque->bottom, OWNER_LOAD_ORDER) - 1;
    Array *array = atomic_load_explicit(&deque->active, OWNER_LOAD_ORDER);
    atomic_store_explicit(&deque->bottom, b, POP_RESERVATION_ORDER);
    POP_FENCE();
    int64_t t = atomic_load_explicit(&deque->top, memory_order_seq_cst);
    deque->cached_top = t;
    int64_t size = b - t;
    if (size < 0) {
        atomic_store_explicit(&deque->bottom, t, POP_RESTORE_ORDER);
        return EMPTY;
    }
    int value = slot_load(array, b);
    if (size > 0) {
        perhaps_shrink(deque, b, t);
        return value;
    }
    if (!cas_top(deque, t, t + 1)) {
        atomic_store_explicit(&deque->bottom, t + 1, POP_RESTORE_ORDER);
        return EMPTY;
    }
    atomic_store_explicit(&deque->bottom, t + 1, POP_RESTORE_ORDER);
    return value;
}

static void *owner_main(void *unused) {
    (void)unused;
    for (int i = 0; i < 3; ++i) owner_results[i] = pop_bottom(&original);
    return NULL;
}

static void *thief_main(void *arg) {
    int index = (int)(intptr_t)arg;
    thief_results[index] = steal(&original, &speculative_values[index]);
    return NULL;
}

static void *borrower_main(void *unused) {
    (void)unused;
    Array *record = try_acquire(3);
    if (record) {
        borrower_acquired = 1;
        atomic_init(&borrower.active, record);
        atomic_init(&borrower.bottom, 0);
        atomic_init(&borrower.top, 0);
        borrower.cached_top = 0;
        borrower.min_log_size = 3;
        int pushed = push_bottom(&borrower, 100);
        assert(pushed == 0);
    }
    return NULL;
}

static void release_chain(Array *cursor) {
    while (cursor) {
        Array *next = cursor->prev;
        release_array(cursor);
        cursor = next;
    }
}

int main(void) {
    small.low_water_mark = INT64_MAX;
    large.low_water_mark = INT64_MAX;
    atomic_init(&small.owned, 1);
    atomic_init(&large.owned, 0);
    for (int i = 0; i < 8; ++i) {
        atomic_init(&small.slots[i], UNINITIALIZED);
        atomic_init(&large.slots[i], UNINITIALIZED);
    }
    atomic_init(&original.active, &small);
    atomic_init(&original.bottom, 0);
    atomic_init(&original.top, 0);
    original.cached_top = 0;
    original.min_log_size = 2;
    for (int i = 0; i < 4; ++i) {
        int pushed = push_bottom(&original, i);
        assert(pushed == 0);
    }
    assert(atomic_load_explicit(&original.active, OWNER_LOAD_ORDER) == &large);

    pthread_t owner, first, second, borrowing_owner;
    int rc = pthread_create(&owner, NULL, owner_main, NULL);
    assert(rc == 0);
    rc = pthread_create(&first, NULL, thief_main, (void *)(intptr_t)0);
    assert(rc == 0);
    rc = pthread_create(&second, NULL, thief_main, (void *)(intptr_t)1);
    assert(rc == 0);
    rc = pthread_create(&borrowing_owner, NULL, borrower_main, NULL);
    assert(rc == 0);
    rc = pthread_join(owner, NULL);
    assert(rc == 0);
    rc = pthread_join(first, NULL);
    assert(rc == 0);
    rc = pthread_join(second, NULL);
    assert(rc == 0);
    rc = pthread_join(borrowing_owner, NULL);
    assert(rc == 0);

#ifdef MODEL_PROBE_SHRINK_SUCCESS
    assert(shrink_succeeded == 0);
#endif
#ifdef MODEL_PROBE_SHRINK_FAILURE
    assert(shrink_failed == 0);
#endif
#ifdef MODEL_PROBE_BORROWER_READ
    assert(!borrower_acquired ||
           (speculative_values[0] != 100 &&
            speculative_values[1] != 100));
#endif

    int seen[4] = {0, 0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        int value = owner_results[i];
        if (value >= 0) { assert(value < 4); ++seen[value]; }
        else assert(value == EMPTY);
    }
    for (int i = 0; i < 2; ++i) {
        int value = thief_results[i];
        if (value >= 0) { assert(value < 4); ++seen[value]; }
        else assert(value == EMPTY || value == ABORT);
    }
    for (int i = 0; i < 4; ++i) {
        int value = pop_bottom(&original);
        if (value >= 0) { assert(value < 4); ++seen[value]; }
        else assert(value == EMPTY);
    }
    for (int i = 0; i < 4; ++i) assert(seen[i] == 1);
    if (borrower_acquired) assert(pop_bottom(&borrower) == 100);
    release_chain(atomic_load_explicit(&original.active, OWNER_LOAD_ORDER));
    if (borrower_acquired)
        release_chain(atomic_load_explicit(&borrower.active, OWNER_LOAD_ORDER));
    assert(atomic_load_explicit(&small.owned, memory_order_relaxed) == 0);
    assert(atomic_load_explicit(&large.owned, memory_order_relaxed) == 0);
    return 0;
}
