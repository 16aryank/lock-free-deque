#pragma once

#include <array>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>


template <class T>
class CircularArray : public std::enable_shared_from_this<CircularArray<T>> {
public:
    explicit CircularArray(std::size_t log_size)
        : log_size_(log_size),
          segment_(std::make_unique<T[]>(std::size_t{1} << log_size)),
          low_water_mark_(std::numeric_limits<std::uint64_t>::max()) {}

    std::size_t size() const noexcept {
        return std::size_t{1} << log_size_;
    }

    std::size_t log_size() const noexcept {
        return log_size_;
    }

    std::shared_ptr<CircularArray<T>> get_prev() const noexcept {
        return prev_;
    }

    T load(std::uint64_t i) const {
        return segment_[index(i)];
    }

    void store(std::uint64_t i, const T& value) {
        segment_[index(i)] = value;
        update_low_water_mark(i);
    }

    void store(std::uint64_t i, T&& value) {
        segment_[index(i)] = std::move(value);
        update_low_water_mark(i);
    }

    std::shared_ptr<CircularArray<T>> grow(std::uint64_t b, std::uint64_t t) const {
        auto new_array = std::make_shared<CircularArray<T>>(log_size_ + 1);
        new_array->prev_ = this->shared_from_this();

        // Values before t do not matter
        for (std::uint64_t i = t; i < b; i++) {
            new_array->store_no_mark(i, load(i));
        }

        return new_array;
    }

    std::shared_ptr<CircularArray<T>> shrink(std::uint64_t b, std::uint64_t t, std::size_t num_shrink) const {
        if (log_size_ == 0) {
            return this->shared_from_this();
        }

        std::uint64_t min_low_water = low_water_mark_;
        auto cursor = this->shared_from_this();
        std::shared_ptr<CircularArray<T>> new_array;

        for (std::size_t i = 0; i < num_shrink; ++i) {
            auto next = cursor->prev_;
            if (!next) {
                break;
            }
            min_low_water = std::min(min_low_water, next->low_water_mark_);
            cursor = std::move(next);
            new_array = cursor;
        }

        if (!new_array) {
            new_array = std::make_shared<CircularArray<T>>(log_size_ - 1);
        }

        new_array->low_water_mark_ = std::min(new_array->low_water_mark_, min_low_water);

        std::uint64_t start = b;
        if (min_low_water != std::numeric_limits<std::uint64_t>::max()) {
            start = std::max(t, min_low_water);
        }

        for (std::uint64_t i = start; i < b; i++) {
            new_array->store_no_mark(i, load(i));
        }

        return new_array;
    }

private:
    // size is guaranteed to be a power of 2
    std::size_t index(std::uint64_t i) const noexcept {
        return static_cast<std::size_t>(i & (size() - 1));
    }

    inline void update_low_water_mark(std::uint64_t i) noexcept {
        low_water_mark_ = std::min(low_water_mark_, i);
    }

    void store_no_mark(std::uint64_t i, const T& value) {
        segment_[index(i)] = value;
    }

    std::size_t log_size_;
    std::unique_ptr<T[]> segment_;
    std::uint64_t low_water_mark_;
    std::shared_ptr<CircularArray<T>> prev_;
};
