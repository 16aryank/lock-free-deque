#pragma once

#include <array>
#include <cstdint>
#include <memory.h>

template <class T>
class CircularArray {
public:
    explicit CircularArray(std::size_t log_size)
        : log_size_(log_size),
          segment_(std::make_unique<T[]>(std::size_t{1} << log_size)) {}

    std::size_t size() const noexcept {
        return std::size_t{1} << log_size_;
    }

    T get(std::uint64_t i) const {
        return segment_[index(i)];
    }

    void put(std::uint64_t i, const T& value) {
        segment_[index(i)] = value;
    }

    void put(std::uint64_t i, T&& value) {
        segment_[index(i)] = std::move(value);
    }

    std::unique_ptr<CircularArray<T>> grow(std::uint64_t b, std::uint64_t t) const {
        auto new_array = std::make_unique<CircularArray<T>>(log_size_ + 1);

        // Values before t do not matter
        for (std::uint64_t i = t; i < b; i++) {
            new_array->put(i, get(i)); 
        }
        return new_array;
    }

private:
    // i % size b/c size is guaranteed to be a power of 2
    std::size_t index(std::uint64_t i) const noexcept {
        return static_cast<std::size_t>(i & (size() - 1));
    }

    std::size_t log_size_;
    std::unique_ptr<T[]> segment_;
};