#pragma once

#include "circular_array.h"
#include <memory>
#include <atomic>
#include <concepts>
#include <utility>
#include <optional>

// Constant factor >= 3
static inline constexpr unsigned int K = 4;

enum class StealState {
    SUCCESS,
    EMPTY,
    ABORT,
};

template <class T>
struct StealResult {
    StealState state_;
    std::optional<T> value_;

    explicit StealResult<T>(StealState state) : 
        state_(state), value_(std::nullopt) {}

    explicit StealResult<T>(T value) :
        state_(StealState::SUCCESS), value_(value) {}
};

template <class T>
requires std::move_constructible<T>
class WorkStealingDeque {
public:
    explicit WorkStealingDeque(std::size_t log_initial_size = 10)
        : bottom_(0),
          top_(0),
          local_top_(0),
          min_log_size_(log_initial_size),
          active_array_(CircularArray<T>::acquire(log_initial_size)) {}

    void push_bottom(T x) {
        std::int64_t b = bottom_.load(std::memory_order_relaxed);
        std::int64_t t = local_top_;
        auto a = active_array_.load(std::memory_order_acquire);
        if (local_top_ == 0 || (b - local_top_ >= a->size() - 1)) {
            t = top_.load(std::memory_order_acquire);
        }

        std::int64_t size = b - t;
        if (size >= a->size() - 1) {
            auto new_array = a->grow(b, t);
            active_array_.store(new_array, std::memory_order_release);
            a = std::move(new_array);
        }

        a->store(b, std::move(x));
        bottom_.store(b + 1, std::memory_order_release);
    }

    StealResult<T> steal() {
        auto t = top_.load(std::memory_order_acquire);
        auto old_array = active_array_.load(std::memory_order_acquire);
        auto b = bottom_.load(std::memory_order_acquire);
        auto a = active_array_.load(std::memory_order_acquire);

        std::int64_t size = b - t;
        if (size <= 0) {
            return StealResult<T>(StealState::EMPTY);
        }

        auto modulus = size % static_cast<std::int64_t>(a->size());
        if (modulus == 0) {
            auto top_snapshot = top_.load(std::memory_order_acquire);
            if (a == old_array && t == top_snapshot) {
                return StealResult<T>(StealState::EMPTY);
            }
            return StealResult<T>(StealState::ABORT);
        }

        T x = a->load(t);
        if (!cas_top(t, t + 1)) {
            return StealResult<T>(StealState::ABORT);
        }
    
        return x;
    }

    std::optional<T> pop_bottom() {
        auto b = bottom_.load(std::memory_order_relaxed) - 1;
        auto a = active_array_.load(std::memory_order_acquire);
        bottom_.store(b, std::memory_order_relaxed);

        auto t = top_.load(std::memory_order_acquire);
        auto size = b - t;

        if (size < 0) {
            bottom_.store(t, std::memory_order_relaxed);
            return std::nullopt;
        }

        T x = a->load(b);
        if (size > 0) {
            perhaps_shrink(b, t);
            return x;
        }

        if (!cas_top(t, t + 1)) {
            bottom_.store(t + 1, std::memory_order_relaxed);
            return std::nullopt;
        }

        bottom_.store(t + 1, std::memory_order_relaxed);
        return x;
    }

private:
    void perhaps_shrink(std::int64_t b, std::int64_t t) {
        auto a = active_array_.load(std::memory_order_acquire);
        auto cursor = a;
        std::size_t num_shrink = 0;

        while (cursor->log_size() > min_log_size_ &&
               (b - t) < (cursor->size() / K)) {
            auto prev = cursor->get_prev();
            if (!prev) {
                break;
            }
            // Don't want to increase the ref count
            cursor = std::move(prev);
            num_shrink++;
        }

        if (num_shrink == 0) {
            return;
        }

        auto new_array = a->shrink(b, t, num_shrink);
        active_array_.store(new_array, std::memory_order_release);
        auto ss = static_cast<std::int64_t>(new_array->size());
        bottom_.store(b + ss, std::memory_order_relaxed);
        auto top_snapshot = top_.load(std::memory_order_acquire);
        if (!cas_top(top_snapshot, top_snapshot + ss)) {
            bottom_.store(b, std::memory_order_relaxed);
        }
    }

    bool cas_top(std::int64_t expected, std::int64_t desired) {
        return top_.compare_exchange_strong(
            expected,
            desired,
            std::memory_order_acq_rel,
            std::memory_order_acquire
        );
    }

    inline bool is_empty() const noexcept {
        return bottom_.load(std::memory_order_acquire) <=
            top_.load(std::memory_order_acquire);       
    }

    std::atomic<std::shared_ptr<CircularArray<T>>> active_array_;
    std::atomic<std::int64_t> bottom_;
    std::atomic<std::int64_t> top_;
    std::int64_t local_top_;
    std::size_t min_log_size_;
};
