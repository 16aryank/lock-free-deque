#pragma once

#include "circular_array.h"
#include <memory.h>
#include <atomic>
#include <concepts>
#include <optional>

enum class StealState {
    SUCCESS,
    EMPTY,
    ABORT,
};

template <class T>
struct StealResult {
    StealState state_;
    std::optional<T> value_;

    explicit StealResult(StealState state) : 
        state_(state), value_(std::nullopt) {}

    explicit StealResult(T value) :
        state_(StealState.SUCCESS), value_(value) {}
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
        unsigned long long b = bottom_.load(std::memory_order_relaxed);
        unsigned long long t = local_top;
        if (local_top_ == -1 || (b - local_top_ >= a->size() - 1)) {
            t = top_.load(std::memory_order_acquire);
        }
        auto* a = active_array_.get();

        unsigned long long size = b - t;
        if (size >= a->size() - 1) {
            auto new_array = a->grow(b, t);
            active_array_ = std::move(new_array);
            a = active_array_.get();
        }

        a->put(b, std::move(x));
        bottom_.store(b + 1, std::memory_order_release);
    }

    StealResult steal() {
        auto t = top_.load(std::memory_order_acquire);
        auto b = bottom_.load(std::memory_order_acquire);
        auto* a = active_array_.get();

        unsigned long long size = b - t;
        if (size <= 0) {
            return StealResult(StealState.EMPTY);
        }

        T x = a->get(t);
        if (!cas_top(t, t + 1)) {
            return StealResult(StealState.ABORT);
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

        T x = a->get(b);
        if (size > 0) {
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
    bool cas_top(std::uint64_t expected, std::uint64_t desired) {
        return top_.compare_exchange_strong(
            expected,
            desired,
            std::memory_order_acq_rel,
            std::memory_order_acquire
        );
    }

    inline bool is_empty() const noexcept {
        return bottom_ <= top_;
    }

    std::atomic<std::uint64_t> bottom_;
    std::atomic<std::uint64_t> top_;
    std::uint64_t local_top_;
    std::unique_ptr<CircularArray<T>> active_array_;
};