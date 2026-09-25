#pragma once

#include "buffer_pool.h"
#include "steal_result.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

// Constant factor >= 3
static inline constexpr unsigned int K = 4;

enum class PushResult {
    SUCCESS,
    NO_BUFFER_ACQUIRED,
};

#ifdef DEQUE_TEST_HOOKS
struct DequeTestHook {
    void (*callback)(void*) = nullptr;
    void* context = nullptr;

    void run() const {
        if (callback) callback(context);
    }
};

struct DequeTestHooks {
    DequeTestHook after_final_array_snapshot;
    DequeTestHook after_speculative_slot_read;
    DequeTestHook after_shrink_bottom_shift;
    DequeTestHook after_shrink_top_snapshot;
    DequeTestHook before_last_pop_cas;
};
#endif

template <LockFreeAtomicValue T>
class WorkStealingDeque {
public:
    explicit WorkStealingDeque(BufferPool<T>& pool, std::size_t log_initial_size = 10)
        : pool_(pool),
          active_array_(pool.try_acquire(log_initial_size)),
          bottom_(0),
          top_(0),
          min_log_size_(log_initial_size) {
        if (active_array_.load(std::memory_order_seq_cst) == nullptr) {
            throw std::bad_alloc{};
        }
    }

    ~WorkStealingDeque() {
        // Workers must be stopped before the deque or its pool is destroyed.
        release_chain(active_array_.load(std::memory_order_seq_cst));
    }

    WorkStealingDeque(const WorkStealingDeque&) = delete;
    WorkStealingDeque& operator=(const WorkStealingDeque&) = delete;

#ifdef DEQUE_TEST_HOOKS
    // Set before starting workers; keep the hook object alive through join.
    void set_test_hooks(DequeTestHooks* hooks) noexcept { test_hooks_ = hooks; }
#endif

    PushResult try_push_bottom(T x) {
        std::int64_t b = bottom_.load(std::memory_order_seq_cst);
        auto* a = active_array_.load(std::memory_order_seq_cst);
        std::int64_t t = top_.load(std::memory_order_seq_cst);

        std::int64_t size = b - t;
        if (size >= a->size() - 1) {
            auto* destination = pool_.try_acquire(a->log_size() + 1);
            if (!destination) {
                return PushResult::NO_BUFFER_ACQUIRED;
            }
            a = a->grow_into(destination, b, t);
            active_array_.store(a, std::memory_order_seq_cst);
        }

        a->store(b, std::move(x));
        bottom_.store(b + 1, std::memory_order_seq_cst);
        return PushResult::SUCCESS;
    }

    StealResult<T> steal() {
        auto t = top_.load(std::memory_order_seq_cst);
        auto* old_array = active_array_.load(std::memory_order_seq_cst);
        auto b = bottom_.load(std::memory_order_seq_cst);
        auto* a = active_array_.load(std::memory_order_seq_cst);
#ifdef DEQUE_TEST_HOOKS
        if (test_hooks_) test_hooks_->after_final_array_snapshot.run();
#endif

        std::int64_t size = b - t;
        if (size <= 0) {
            return StealResult<T>{StealState::EMPTY};
        }

        auto modulus = size % static_cast<std::int64_t>(a->size());
        if (modulus == 0) {
            auto top_snapshot = top_.load(std::memory_order_seq_cst);
            if (a == old_array && t == top_snapshot) {
                return StealResult<T>{StealState::EMPTY};
            }
            return StealResult<T>{StealState::ABORT};
        }

        T x = a->load(t);
#ifdef DEQUE_TEST_HOOKS
        if (test_hooks_) test_hooks_->after_speculative_slot_read.run();
#endif
        if (!cas_top(t, t + 1)) {
            return StealResult<T>{StealState::ABORT};
        }
    
        return StealResult<T>{x};
    }

    std::optional<T> pop_bottom() {
        auto b = bottom_.load(std::memory_order_seq_cst) - 1;
        auto* a = active_array_.load(std::memory_order_seq_cst);
        bottom_.store(b, std::memory_order_seq_cst);

        auto t = top_.load(std::memory_order_seq_cst);
        auto size = b - t;

        if (size < 0) {
            bottom_.store(t, std::memory_order_seq_cst);
            return std::nullopt;
        }

        T x = a->load(b);
        if (size > 0) {
            perhaps_shrink(b, t);
            return x;
        }

#ifdef DEQUE_TEST_HOOKS
        if (test_hooks_) test_hooks_->before_last_pop_cas.run();
#endif
        if (!cas_top(t, t + 1)) {
            bottom_.store(t + 1, std::memory_order_seq_cst);
            return std::nullopt;
        }

        bottom_.store(t + 1, std::memory_order_seq_cst);
        return x;
    }

private:
    void release_chain(CircularArray<T>* cursor) noexcept {
        while (cursor) {
            auto* next = cursor->prev_;
            cursor->prev_ = nullptr;
            pool_.release(cursor);
            cursor = next;
        }
    }

    void perhaps_shrink(std::int64_t b, std::int64_t t) {
        auto* a = active_array_.load(std::memory_order_seq_cst);
        auto* cursor = a;
        std::size_t num_shrink = 0;

        while (cursor->log_size() > min_log_size_ &&
               (b - t) < (cursor->size() / K)) {
            auto* prev = cursor->get_prev();
            if (!prev) {
                break;
            }
            cursor = prev;
            num_shrink++;
        }

        if (num_shrink == 0) {
            return;
        }

        auto* new_array = a->shrink(b, t, num_shrink);
        active_array_.store(new_array, std::memory_order_seq_cst);
        auto ss = static_cast<std::int64_t>(new_array->size());
        bottom_.store(b + ss, std::memory_order_seq_cst);
#ifdef DEQUE_TEST_HOOKS
        if (test_hooks_) test_hooks_->after_shrink_bottom_shift.run();
#endif
        auto top_snapshot = top_.load(std::memory_order_seq_cst);
#ifdef DEQUE_TEST_HOOKS
        if (test_hooks_) test_hooks_->after_shrink_top_snapshot.run();
#endif
        if (!cas_top(top_snapshot, top_snapshot + ss)) {
            bottom_.store(b, std::memory_order_seq_cst);
        }

        // The top CAS (or failure-path bottom restoration) invalidates stale
        // claims before these buffers become available to other deques.
        auto* discarded = a;
        while (discarded != new_array) {
            auto* next = discarded->prev_;
            discarded->prev_ = nullptr;
            pool_.release(discarded);
            discarded = next;
        }
    }

    bool cas_top(std::int64_t expected, std::int64_t desired) {
        return top_.compare_exchange_strong(
            expected,
            desired,
            std::memory_order_seq_cst,
            std::memory_order_seq_cst
        );
    }

    static_assert(std::atomic<CircularArray<T>*>::is_always_lock_free);
    static_assert(std::atomic<std::int64_t>::is_always_lock_free);

    BufferPool<T>& pool_;
    std::atomic<CircularArray<T>*> active_array_;
    std::atomic<std::int64_t> bottom_;
    std::atomic<std::int64_t> top_;
    std::size_t min_log_size_;
#ifdef DEQUE_TEST_HOOKS
    DequeTestHooks* test_hooks_{ nullptr };
#endif
};
