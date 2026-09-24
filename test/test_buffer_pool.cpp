#include "buffer_pool.h"

#include <gtest/gtest.h>
#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

namespace {

TEST(BufferPoolTest, FixedRecordsExhaustAndReuseWithinTheirSizeClass) {
    BufferPool<int> pool({{2, 2}, {3, 1}});

    auto* first = pool.try_acquire(2);
    auto* second = pool.try_acquire(2);
    auto* larger = pool.try_acquire(3);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_NE(larger, nullptr);
    EXPECT_NE(first, second);
    EXPECT_EQ(first->size(), 4u);
    EXPECT_EQ(second->size(), 4u);
    EXPECT_EQ(larger->size(), 8u);
    EXPECT_EQ(pool.try_acquire(2), nullptr);
    EXPECT_EQ(pool.try_acquire(3), nullptr);
    EXPECT_EQ(pool.try_acquire(4), nullptr);

    first->store(5, 42); // Physical index wraps; the same slot remains live.
    pool.release(first);
    auto* reused = pool.try_acquire(2);
    ASSERT_EQ(reused, first);
    EXPECT_EQ(reused->load(5), 42);
    EXPECT_EQ(pool.try_acquire(2), nullptr);

    pool.release(reused);
    pool.release(second);
    pool.release(larger);
}

TEST(BufferPoolTest, InvalidConfigurationFailsDuringSetup) {
    EXPECT_THROW((BufferPool<int>({})), std::invalid_argument);
    EXPECT_THROW((BufferPool<int>({{2, 0}})), std::invalid_argument);
    EXPECT_THROW((BufferPool<int>({{2, 1}, {2, 1}})), std::invalid_argument);
    EXPECT_THROW((BufferPool<int>({{BufferPool<int>::max_log_size() + 1, 1}})),
                 std::invalid_argument);
}

TEST(BufferPoolTest, ConcurrentClaimsNeverOwnTheSameRecordTwice) {
    constexpr std::size_t kRecords = 3;
    constexpr int kThreads = 8;
    constexpr int kAttempts = 1000;
    BufferPool<int> pool({{2, kRecords}});

    std::vector<CircularArray<int>*> addresses;
    for (std::size_t i = 0; i < kRecords; ++i) {
        auto* buffer = pool.try_acquire(2);
        ASSERT_NE(buffer, nullptr);
        addresses.push_back(buffer);
    }
    EXPECT_EQ(pool.try_acquire(2), nullptr);
    for (auto* buffer : addresses) {
        pool.release(buffer);
    }

    std::atomic<int> in_use[kRecords]{};
    std::atomic<int> double_claims{0};
    std::atomic<int> successes{0};
    std::vector<std::thread> workers;
    for (int thread = 0; thread < kThreads; ++thread) {
        workers.emplace_back([&] {
            for (int attempt = 0; attempt < kAttempts; ++attempt) {
                auto* buffer = pool.try_acquire(2);
                if (!buffer) {
                    std::this_thread::yield();
                    continue;
                }
                std::size_t index = 0;
                while (addresses[index] != buffer) {
                    ++index;
                }
                if (in_use[index].fetch_add(1) != 0) {
                    double_claims.fetch_add(1);
                }
                successes.fetch_add(1);
                std::this_thread::yield();
                in_use[index].fetch_sub(1);
                pool.release(buffer);
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    EXPECT_GT(successes.load(), 0);
    EXPECT_EQ(double_claims.load(), 0);
    for (std::size_t i = 0; i < kRecords; ++i) {
        EXPECT_EQ(in_use[i].load(), 0);
    }
    for (std::size_t i = 0; i < kRecords; ++i) {
        EXPECT_NE(pool.try_acquire(2), nullptr);
    }
    EXPECT_EQ(pool.try_acquire(2), nullptr);
    for (auto* buffer : addresses) {
        pool.release(buffer);
    }
}

} // namespace
