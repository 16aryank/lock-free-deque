#pragma once

#include "circular_array.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

// Construct before starting workers. Destroy only after all operations have
// finished and every acquired buffer has been returned.
template <LockFreeAtomicValue T>
class BufferPool {
public:
    struct SizeClassConfig {
        std::size_t log_size;
        std::size_t count;
    };

    explicit BufferPool(std::vector<SizeClassConfig> config) {
        if (config.empty()) {
            throw std::invalid_argument("a buffer pool needs at least one size class");
        }

        std::size_t total = 0;
        std::size_t largest_log_size = 0;
        for (const auto& entry : config) {
            if (entry.count == 0 || entry.log_size > max_log_size()) {
                throw std::invalid_argument("invalid buffer size class");
            }
            if (entry.count > std::numeric_limits<std::size_t>::max() - total) {
                throw std::length_error("too many pool records");
            }
            total += entry.count;
            largest_log_size = std::max(largest_log_size, entry.log_size);
        }

        ranges_.resize(largest_log_size + 1);
        records_.reserve(total);
        for (const auto& entry : config) {
            auto& range = ranges_[entry.log_size];
            if (range.count != 0) {
                throw std::invalid_argument("duplicate buffer size class");
            }
            range.begin = records_.size();
            range.count = entry.count;
            for (std::size_t i = 0; i < entry.count; ++i) {
                records_.emplace_back(std::make_unique<Record>(entry.log_size));
            }
        }
    }

    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    ~BufferPool() {
        // External shutdown must join workers and return all buffers first.
        for (const auto& record : records_) {
            assert(record->state.load(std::memory_order_relaxed) == kAvailable);
        }
    }

    // One strong CAS per permanent record. A successful claim acquires the
    // previous owner's release so reset_for_reuse cannot race with that
    // owner's metadata access. A failed scan inspects no record metadata.
    CircularArray<T>* try_acquire(std::size_t log_size) noexcept {
        if (log_size >= ranges_.size()) {
            return nullptr;
        }
        const auto range = ranges_[log_size];
        for (std::size_t i = range.begin; i < range.begin + range.count; ++i) {
            auto& record = *records_[i];
            std::uint32_t expected = kAvailable;
            if (record.state.compare_exchange_strong(
                    expected, kOwned, std::memory_order_acquire,
                    std::memory_order_relaxed)) {
                record.reset_for_reuse();
                return &record;
            }
        }
        return nullptr;
    }

    // The caller owns buffer and must not access its mutable metadata afterward.
    // In shrink, the SC top CAS (or its failed-path SC bottom restoration)
    // precedes this release. A borrower's acquire claim precedes its SC slot
    // writes. If a stale thief reads one of those writes, its SC slot read
    // acquires that write before attempting the SC top CAS; the invalidation
    // is therefore visible to that claim. This edge matters for reuse as
    // well as for transfer of prev_ and low_water_mark_.
    void release(CircularArray<T>* buffer) noexcept {
        static_cast<Record*>(buffer)->state.store(kAvailable, std::memory_order_release);
    }

    static constexpr std::size_t max_log_size() noexcept {
        constexpr auto size_limit = std::numeric_limits<std::size_t>::digits - 2;
        constexpr auto index_limit = std::numeric_limits<std::int64_t>::digits - 2;
        return size_limit < index_limit ? size_limit : index_limit;
    }

private:
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
                  "pool flags must be lock-free");

    static constexpr std::uint32_t kAvailable = 0;
    static constexpr std::uint32_t kOwned = 1;

    struct Record : CircularArray<T> {
        explicit Record(std::size_t log_size) : CircularArray<T>(log_size) {}
        std::atomic<std::uint32_t> state{kAvailable};
    };

    struct SizeClassRange {
        std::size_t begin = 0;
        std::size_t count = 0;
    };

    std::vector<SizeClassRange> ranges_;
    std::vector<std::unique_ptr<Record>> records_;
};
