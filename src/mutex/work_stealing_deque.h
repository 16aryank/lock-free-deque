#pragma once

#include "../steal_result.h"
#include <cstddef>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace mutex_deque {

// Circular-buffer baseline: one mutex covers each complete operation,
// including resizing. The owner uses the bottom; thieves use the top.
template <class T>
class WorkStealingDeque {
public:
    explicit WorkStealingDeque(std::size_t log_initial_size = 10)
        : buffer_(capacity(log_initial_size)), min_capacity_(buffer_.size()) {}

    void push_bottom(T value) {
        std::lock_guard lock(mutex_);
        // Leave one slot unused, matching the nonblocking implementation.
        if (size_ == buffer_.size() - 1) {
            if (buffer_.size() > buffer_.max_size() / 2)
                throw std::length_error("deque capacity overflow");
            resize(buffer_.size() * 2);
        }
        buffer_[index(size_)] = std::move(value);
        ++size_;
    }

    std::optional<T> pop_bottom() {
        std::lock_guard lock(mutex_);
        if (size_ == 0) return std::nullopt;
        T value = buffer_[index(size_ - 1)];
        --size_;
        // As in the original deque, only owner pops trigger shrinking.
        auto target = buffer_.size();
        while (target > min_capacity_ && size_ < target / 4) target /= 2;
        if (target != buffer_.size()) resize(target);
        return value;
    }

    StealResult<T> steal() {
        std::lock_guard lock(mutex_);
        if (size_ == 0) return StealResult<T>{StealState::EMPTY};
        T value = buffer_[top_];
        top_ = index(1);
        --size_;
        return StealResult<T>{value};
    }

private:
    static std::size_t capacity(std::size_t log_size) {
        if (log_size >= std::numeric_limits<std::size_t>::digits)
            throw std::invalid_argument("initial capacity exponent is too large");
        return std::size_t{1} << log_size;
    }

    std::size_t index(std::size_t offset) const {
        return (top_ + offset) & (buffer_.size() - 1);
    }

    void resize(std::size_t new_capacity) {
        std::vector<T> replacement(new_capacity);
        for (std::size_t i = 0; i < size_; ++i) replacement[i] = buffer_[index(i)];
        buffer_.swap(replacement);
        top_ = 0;
    }

    std::mutex mutex_;
    std::vector<T> buffer_;
    const std::size_t min_capacity_;
    std::size_t top_ = 0;
    std::size_t size_ = 0;
};

} // namespace mutex_deque
