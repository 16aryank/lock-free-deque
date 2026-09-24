#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <type_traits>

template <class T>
concept LockFreeAtomicValue =
    std::is_trivially_copyable_v<T> &&
    std::is_same_v<T, std::remove_cv_t<T>> &&
    std::is_copy_constructible_v<T> &&
    std::is_move_constructible_v<T> &&
    std::is_copy_assignable_v<T> &&
    std::is_move_assignable_v<T> &&
    std::atomic<T>::is_always_lock_free;

template <LockFreeAtomicValue T> class BufferPool;
template <LockFreeAtomicValue T> class WorkStealingDeque;

template <LockFreeAtomicValue T>
class CircularArray {
public:
    explicit CircularArray(std::size_t log_size)
        : log_size_(log_size),
          segment_(std::make_unique<std::atomic<T>[]>(std::size_t{1} << log_size)) {}

    std::size_t size() const noexcept { return std::size_t{1} << log_size_; }
    std::size_t log_size() const noexcept { return log_size_; }
    CircularArray* get_prev() const noexcept { return prev_; }

    T load(std::uint64_t i) const {
        return segment_[index(i)].load(std::memory_order_seq_cst);
    }

    void store(std::uint64_t i, T value) {
        segment_[index(i)].store(value, std::memory_order_seq_cst);
        low_water_mark_ = std::min(low_water_mark_, i);
    }

    // The caller owns destination. Its slots and metadata are prepared before
    // the deque publishes it as active.
    CircularArray* grow_into(CircularArray* destination,
                             std::uint64_t b, std::uint64_t t) {
        destination->prev_ = this;
        for (std::uint64_t i = t; i < b; ++i) {
            destination->store_no_mark(i, load(i));
        }
        return destination;
    }

    CircularArray* shrink(std::uint64_t b, std::uint64_t t,
                          std::size_t num_shrink) {
        std::uint64_t min_low_water = low_water_mark_;
        auto* destination = this;
        for (std::size_t i = 0; i < num_shrink; ++i) {
            destination = destination->prev_;
            min_low_water = std::min(min_low_water, destination->low_water_mark_);
        }

        destination->low_water_mark_ =
            std::min(destination->low_water_mark_, min_low_water);
        std::uint64_t start = b;
        if (min_low_water != std::numeric_limits<std::uint64_t>::max()) {
            start = std::max(t, min_low_water);
        }
        for (std::uint64_t i = start; i < b; ++i) {
            destination->store_no_mark(i, load(i));
        }
        return destination;
    }

private:
    std::size_t index(std::uint64_t i) const noexcept {
        return static_cast<std::size_t>(i & (size() - 1));
    }

    void store_no_mark(std::uint64_t i, T value) {
        segment_[index(i)].store(value, std::memory_order_seq_cst);
    }

    void reset_for_reuse() noexcept {
        low_water_mark_ = std::numeric_limits<std::uint64_t>::max();
        prev_ = nullptr;
    }

    const std::size_t log_size_;
    const std::unique_ptr<std::atomic<T>[]> segment_;
    std::uint64_t low_water_mark_ = std::numeric_limits<std::uint64_t>::max();
    CircularArray* prev_ = nullptr;

    friend class BufferPool<T>;
    friend class WorkStealingDeque<T>;
};
