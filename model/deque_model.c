// Bounded C11 translation of the production deque/pool and steal retry protocol.
// See README.md for bounds and differences from the C++ implementation.
#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>

enum {
    K = 4,
    UNINITIALIZED = -99,
    EMPTY = -1,
    ABORT = -2,
    NO_BUFFER = -3,
    MODEL_RETRY_ATTEMPTS = 2,
    MODEL_INITIAL_WINDOW = 4,
    MODEL_MAX_WINDOW = 16
};

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
static unsigned thief_attempts[2];
static unsigned thief_windows[2];
static unsigned thief_batches[2];
static int borrower_acquired;

static int64_t capacity(const Array *array) {
    return INT64_C(1) << array->log_size;
}

static int slot_load(const Array *array, int64_t i) {
    return atomic_load_explicit(&array->slots[i & (capacity(array) - 1)],
                                memory_order_seq_cst);
}

static void slot_store_no_mark(Array *array, int64_t i, int value) {
    atomic_store_explicit(&array->slots[i & (capacity(array) - 1)], value,
                          memory_order_seq_cst);
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
                                                  memory_order_acquire,
                                                  memory_order_relaxed)) {
        return NULL;
    }
    record->prev = NULL;
    record->low_water_mark = INT64_MAX;
    return record;
}

static void release_array(Array *array) {
    array->prev = NULL;
    atomic_store_explicit(&array->owned, 0, memory_order_release);
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
    int64_t b = atomic_load_explicit(&deque->bottom, memory_order_relaxed);
    Array *array = atomic_load_explicit(&deque->active, memory_order_relaxed);
    if (b - deque->cached_top >= capacity(array) - 1) {
        int64_t t = atomic_load_explicit(&deque->top, memory_order_seq_cst);
        deque->cached_top = t;
        if (b - t >= capacity(array) - 1) {
            Array *destination = try_acquire(array->log_size + 1);
            if (!destination) return NO_BUFFER;
            array = grow_into(array, destination, b, t);
            atomic_store_explicit(&deque->active, array, memory_order_seq_cst);
        }
    }
    slot_store(array, b, value);
    atomic_store_explicit(&deque->bottom, b + 1, memory_order_seq_cst);
    return 0;
}

static int steal(Deque *deque) {
    int64_t t = atomic_load_explicit(&deque->top, memory_order_seq_cst);
    Array *old_array = atomic_load_explicit(&deque->active,
                                             memory_order_seq_cst);
    int64_t b = atomic_load_explicit(&deque->bottom, memory_order_seq_cst);
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
    int value = slot_load(array, t);
    return cas_top(deque, t, t + 1) ? value : ABORT;
}

typedef struct {
    unsigned window;
    unsigned raw_attempts;
} RetryState;

static int steal_with_retry(Deque *deque, RetryState *state) {
    for (unsigned attempt = 0; attempt < MODEL_RETRY_ATTEMPTS; ++attempt) {
        // Jitter and CPU pause touch no shared state, so the model elides
        // their elapsed time while retaining every fresh raw steal attempt.
        if (state->window != 0) {
            assert(state->window >= MODEL_INITIAL_WINDOW);
            assert(state->window <= MODEL_MAX_WINDOW);
        }
        int result = steal(deque);
        ++state->raw_attempts;
        if (result != ABORT) {
            state->window = 0;
            return result;
        }
        state->window = state->window == 0
            ? MODEL_INITIAL_WINDOW
            : state->window * 2 < MODEL_MAX_WINDOW
                ? state->window * 2 : MODEL_MAX_WINDOW;
    }
    return ABORT;
}

static void perhaps_shrink(Deque *deque, int64_t b, int64_t t) {
    Array *array = atomic_load_explicit(&deque->active, memory_order_relaxed);
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
        atomic_store_explicit(&deque->bottom, b, memory_order_seq_cst);
    }

    Array *discarded = array;
    while (discarded != new_array) {
        Array *next = discarded->prev;
        release_array(discarded);
        discarded = next;
    }
}

static int pop_bottom(Deque *deque) {
    int64_t b = atomic_load_explicit(&deque->bottom, memory_order_relaxed) - 1;
    Array *array = atomic_load_explicit(&deque->active, memory_order_relaxed);
    atomic_store_explicit(&deque->bottom, b, memory_order_seq_cst);
    int64_t t = atomic_load_explicit(&deque->top, memory_order_seq_cst);
    deque->cached_top = t;
    int64_t size = b - t;
    if (size < 0) {
        atomic_store_explicit(&deque->bottom, t, memory_order_seq_cst);
        return EMPTY;
    }
    int value = slot_load(array, b);
    if (size > 0) {
        perhaps_shrink(deque, b, t);
        return value;
    }
    if (!cas_top(deque, t, t + 1)) {
        atomic_store_explicit(&deque->bottom, t + 1, memory_order_seq_cst);
        return EMPTY;
    }
    atomic_store_explicit(&deque->bottom, t + 1, memory_order_seq_cst);
    return value;
}

static void *owner_main(void *unused) {
    (void)unused;
    for (int i = 0; i < 3; ++i) owner_results[i] = pop_bottom(&original);
    return NULL;
}

static void *thief_main(void *arg) {
    int index = (int)(intptr_t)arg;
    RetryState state = {0, 0};
    int result = steal_with_retry(&original, &state);
    unsigned batches = 1;
    if (result == ABORT) {
        // The same worker retries later against the same victim, retaining
        // the window established by the exhausted first batch.
        assert(state.window == 2 * MODEL_INITIAL_WINDOW);
        result = steal_with_retry(&original, &state);
        ++batches;
    }
    thief_results[index] = result;
    thief_attempts[index] = state.raw_attempts;
    thief_windows[index] = state.window;
    thief_batches[index] = batches;
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
    assert(atomic_load_explicit(&original.active, memory_order_relaxed) == &large);

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

    int seen[4] = {0, 0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        int value = owner_results[i];
        if (value >= 0) { assert(value < 4); ++seen[value]; }
        else assert(value == EMPTY);
    }
    for (int i = 0; i < 2; ++i) {
        int value = thief_results[i];
        assert(thief_batches[i] >= 1 && thief_batches[i] <= 2);
        assert(thief_attempts[i] >= thief_batches[i]);
        assert(thief_attempts[i] <= thief_batches[i] * MODEL_RETRY_ATTEMPTS);
        assert(thief_windows[i] == (value == ABORT ? MODEL_MAX_WINDOW : 0));
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
    release_chain(atomic_load_explicit(&original.active, memory_order_relaxed));
    if (borrower_acquired)
        release_chain(atomic_load_explicit(&borrower.active, memory_order_relaxed));
    assert(atomic_load_explicit(&small.owned, memory_order_relaxed) == 0);
    assert(atomic_load_explicit(&large.owned, memory_order_relaxed) == 0);
    return 0;
}
