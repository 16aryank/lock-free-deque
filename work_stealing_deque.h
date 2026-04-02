#pragma once

#include "circular_array.h"
#include <memory>
#include <atomic>
#include <concepts>
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
class WorkStealingDeque {
public:
    explicit WorkStealingDeque(std::size_t log_initial_size = 10)
        : bottom_(0),
          top_(0),
          local_top_(0),
          active_array_(std::make_unique<CircularArray<T>>(log_initial_size)) {}

    void push_bottom(T x) {
        std::int64_t b = bottom_.load(std::memory_order_relaxed);
        std::int64_t t = local_top_;
        auto* a = active_array_.get();
        if (local_top_ == 0 || (b - local_top_ >= a->size() - 1)) {
            t = top_.load(std::memory_order_acquire);
        }

        std::int64_t size = b - t;
        if (size >= a->size() - 1) {
            a->grow(b, t);
            active_array_ = std::move(a);
            a = active_array_.get();
        }

        a->store(b, std::move(x));
        bottom_.store(b + 1, std::memory_order_release);
    }

    StealResult<T> steal() {
        auto t = top_.load(std::memory_order_acquire);
        auto b = bottom_.load(std::memory_order_acquire);
        auto* a = active_array_.get();

        std::int64_t size = b - t;
        if (size <= 0) {
            return StealResult<T>(StealState::EMPTY);
        }

        T x = a->load(t);
        if (!cas_top(t, t + 1)) {
            return StealResult<T>(StealState::ABORT);
        }
    
        return x;
    }

    std::optional<T> pop_bottom() {
        auto b = bottom_.load(std::memory_order_relaxed) - 1;
        auto* a = active_array_.get();
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
        auto* a = active_array_.get();
        if ((b - t) < (a->size() / K)) {
            CircularArray aa = a->shrink(b, t);
            active_array_ = aa;
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

    std::unique_ptr<CircularArray<T>> active_array_;
    std::atomic<std::int64_t> bottom_;
    std::atomic<std::int64_t> top_;
    std::int64_t local_top_;
};