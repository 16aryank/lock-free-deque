#define private public
#include "work_stealing_deque.h"
#undef private

#include <gtest/gtest.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <new>
#include <optional>
#include <thread>
#include <string>
#include <type_traits>

namespace {

struct LargeValue {
    char bytes[1024];
};

struct MoveOnlyValue {
    int value;
    MoveOnlyValue() = default;
    MoveOnlyValue(const MoveOnlyValue&) = delete;
    MoveOnlyValue(MoveOnlyValue&&) = default;
    MoveOnlyValue& operator=(const MoveOnlyValue&) = delete;
    MoveOnlyValue& operator=(MoveOnlyValue&&) = default;
};

template <class T>
concept SupportedDequeValue = requires {
    typename CircularArray<T>;
    typename WorkStealingDeque<T>;
};

static_assert(SupportedDequeValue<int>);
static_assert(SupportedDequeValue<int*>);
static_assert(!SupportedDequeValue<LargeValue>);
static_assert(!SupportedDequeValue<std::string>);
static_assert(!SupportedDequeValue<MoveOnlyValue>);
static_assert(!SupportedDequeValue<const int>);
static_assert(!SupportedDequeValue<volatile int>);
static_assert(!SupportedDequeValue<int&>);
static_assert(!SupportedDequeValue<void>);
static_assert(std::is_same_v<decltype(std::declval<CircularArray<int>>().get_prev()),
                             CircularArray<int>*>);
static_assert(std::is_same_v<decltype(std::declval<WorkStealingDeque<int>>().active_array_),
                             std::atomic<CircularArray<int>*>>);

constexpr std::size_t kDefaultLogSize = 4;

TEST(WorkStealingDequeTest, ContendedFieldsOccupySeparateCacheLines) {
    BufferPool<int> pool({{2, 1}});
    WorkStealingDeque<int> deque(pool, 2);
    const auto line = [](const auto& field) {
        return reinterpret_cast<std::uintptr_t>(&field) / kDequeInterferenceSize;
    };

    EXPECT_GE(kDequeInterferenceSize, std::hardware_destructive_interference_size);
    EXPECT_GE(alignof(WorkStealingDeque<int>), kDequeInterferenceSize);
    EXPECT_EQ(sizeof(deque) % kDequeInterferenceSize, 0u);
    EXPECT_NE(line(deque.active_array_), line(deque.bottom_));
    EXPECT_NE(line(deque.bottom_), line(deque.top_));
    EXPECT_NE(line(deque.top_), line(deque.cached_top_));
}

TEST(WorkStealingDequeTest, StealReturnsEmptySuccessAndAbort) {
    BufferPool<int> pool({{2, 1}, {kDefaultLogSize, 1}});
    WorkStealingDeque<int> deque{pool, kDefaultLogSize};

    // Empty case.
    auto empty = deque.steal();
    EXPECT_EQ(empty.state_, StealState::EMPTY);

    // Success case.
    ASSERT_EQ(deque.try_push_bottom(42), PushResult::SUCCESS);
    auto success = deque.steal();
    EXPECT_EQ(success.state_, StealState::SUCCESS);
    ASSERT_TRUE(success.value_.has_value());
    EXPECT_EQ(success.value_.value(), 42);

    // Abort case: race two stealers on a single element.
    bool saw_abort = false;
    bool saw_success = false;
    constexpr int kAttempts = 10000;
    for (int i = 0; i < kAttempts && !saw_abort; ++i) {
        WorkStealingDeque<int> d(pool, 2);
        ASSERT_EQ(d.try_push_bottom(i), PushResult::SUCCESS);

        std::atomic<int> ready{0};
        StealResult<int> r1(StealState::EMPTY);
        StealResult<int> r2(StealState::EMPTY);

        auto worker = [&](StealResult<int>* out) {
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (ready.load(std::memory_order_acquire) < 2) {
                std::this_thread::yield();
            }
            *out = d.steal();
        };

        std::thread t1(worker, &r1);
        std::thread t2(worker, &r2);
        t1.join();
        t2.join();

        if (r1.state_ == StealState::SUCCESS || r2.state_ == StealState::SUCCESS) {
            saw_success = true;
        }
        if (r1.state_ == StealState::ABORT || r2.state_ == StealState::ABORT) {
            saw_abort = true;
        }
    }

    EXPECT_TRUE(saw_success);
    EXPECT_TRUE(saw_abort);
}

TEST(WorkStealingDequeTest, StressAroundGrowShrinkThresholds) {
    BufferPool<int> pool({{4, 1}, {5, 1}});
    WorkStealingDeque<int> deque(pool, 4); // size 16
    std::deque<int> model;

    const int iterations = 2000;
    int next_value = 0;
    for (int i = 0; i < iterations; ++i) {
        // Cross the growth threshold on every cycle, then shrink again.
        while (model.size() < 20) {
            const int value = next_value++;
            ASSERT_EQ(deque.try_push_bottom(value), PushResult::SUCCESS);
            model.push_back(value);
        }

        // Pop down below shrink threshold (size < N/K).
        while (model.size() > 3) {
            auto v = deque.pop_bottom();
            ASSERT_TRUE(v.has_value());
            EXPECT_EQ(v.value(), model.back());
            model.pop_back();
        }

        // Ensure contents still correct.
        while (!model.empty()) {
            auto v = deque.pop_bottom();
            ASSERT_TRUE(v.has_value());
            EXPECT_EQ(v.value(), model.back());
            model.pop_back();
        }
    }
}

TEST(WorkStealingDequeTest, MultiShrinkSkipsIntermediateArrays) {
    BufferPool<int> pool({{2, 1}, {3, 1}, {4, 1}, {5, 1},
                          {6, 1}, {7, 1}, {8, 1}});
    WorkStealingDeque<int> deque(pool, 2); // size 4

    // Force multiple grows.
    for (int i = 0; i < 200; ++i) {
        ASSERT_EQ(deque.try_push_bottom(i), PushResult::SUCCESS);
    }

    std::array<CircularArray<int>*, 9> by_log{};
    for (auto* cursor = deque.active_array_.load(); cursor; cursor = cursor->get_prev()) {
        by_log[cursor->log_size()] = cursor;
    }
    ASSERT_NE(by_log[8], nullptr);

    // Thieves drain the deque without owner pops, leaving one owner pop to
    // shrink across every retained intermediate array in one operation.
    for (int i = 0; i < 198; ++i) {
        auto stolen = deque.steal();
        ASSERT_EQ(stolen.state_, StealState::SUCCESS);
        ASSERT_TRUE(stolen.value_.has_value());
        EXPECT_EQ(*stolen.value_, i);
    }
    auto popped = deque.pop_bottom();
    ASSERT_TRUE(popped.has_value());
    EXPECT_EQ(*popped, 199);
    EXPECT_EQ(deque.active_array_.load(), by_log[2]);
    EXPECT_EQ(pool.try_acquire(2), nullptr);

    for (std::size_t log_size = 3; log_size <= 8; ++log_size) {
        auto* released = pool.try_acquire(log_size);
        ASSERT_EQ(released, by_log[log_size]);
        EXPECT_EQ(pool.try_acquire(log_size), nullptr);
        pool.release(released);
    }
    popped = deque.pop_bottom();
    ASSERT_TRUE(popped.has_value());
    EXPECT_EQ(*popped, 198);
}

TEST(WorkStealingDequeTest, PoolReusesBuffersAcrossDeques) {
    constexpr std::size_t log_size = 3;
    BufferPool<int> pool({{log_size, 1}});
    CircularArray<int>* freed = nullptr;
    {
        WorkStealingDeque<int> d(pool, log_size);
        freed = d.active_array_.load(std::memory_order_seq_cst);
        EXPECT_EQ(pool.try_acquire(log_size), nullptr);
    }

    WorkStealingDeque<int> d2(pool, log_size);
    auto* reused = d2.active_array_.load(std::memory_order_seq_cst);
    EXPECT_EQ(reused, freed);
}

TEST(WorkStealingDequeTest, DiscardedBufferIsReusableBeforeOriginalDequeShutdown) {
    BufferPool<int> pool({{2, 1}, {3, 1}, {4, 1}, {5, 1}});
    std::optional<WorkStealingDeque<int>> reused;
    CircularArray<int>* discarded = nullptr;
    {
        WorkStealingDeque<int> deque(pool, 2);
        for (int i = 0; i < 20; ++i) {
            ASSERT_EQ(deque.try_push_bottom(i), PushResult::SUCCESS);
        }
        discarded = deque.active_array_.load();
        ASSERT_EQ(discarded->log_size(), 5u);

        for (int i = 19; i > 0; --i) {
            auto value = deque.pop_bottom();
            ASSERT_TRUE(value.has_value());
            EXPECT_EQ(*value, i);
        }
        EXPECT_EQ(deque.active_array_.load()->log_size(), 2u);
        reused.emplace(pool, 5);
        EXPECT_EQ(reused->active_array_.load(), discarded);
        EXPECT_EQ(pool.try_acquire(5), nullptr);

        auto remaining = deque.pop_bottom();
        ASSERT_TRUE(remaining.has_value());
        EXPECT_EQ(*remaining, 0);
    }

    // Destroying the first deque must not return a buffer now owned by reused.
    EXPECT_EQ(pool.try_acquire(5), nullptr);
    ASSERT_EQ(reused->try_push_bottom(42), PushResult::SUCCESS);
    auto value = reused->pop_bottom();
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 42);
    reused.reset();
    auto* available = pool.try_acquire(5);
    ASSERT_EQ(available, discarded);
    pool.release(available);
}

TEST(WorkStealingDequeTest, MissingInitialBufferFailsDuringSetup) {
    BufferPool<int> pool({{3, 1}});
    EXPECT_THROW((WorkStealingDeque<int>(pool, 2)), std::bad_alloc);
    auto* available = pool.try_acquire(3);
    ASSERT_NE(available, nullptr);
    pool.release(available);
}

TEST(WorkStealingDequeTest, MissingGrowthBufferLeavesQueuedWorkIntact) {
    BufferPool<int> pool({{2, 1}});
    WorkStealingDeque<int> deque(pool, 2);
    for (int value = 0; value < 3; ++value) {
        ASSERT_EQ(deque.try_push_bottom(value), PushResult::SUCCESS);
    }

    auto* before_array = deque.active_array_.load();
    const auto before_bottom = deque.bottom_.load();
    const auto before_top = deque.top_.load();
    EXPECT_EQ(deque.try_push_bottom(3), PushResult::NO_BUFFER_ACQUIRED);
    EXPECT_EQ(deque.active_array_.load(), before_array);
    EXPECT_EQ(deque.bottom_.load(), before_bottom);
    EXPECT_EQ(deque.top_.load(), before_top);
    EXPECT_EQ(deque.active_array_.load()->log_size(), 2u);
    for (int value = 2; value >= 0; --value) {
        auto popped = deque.pop_bottom();
        ASSERT_TRUE(popped.has_value());
        EXPECT_EQ(*popped, value);
    }
    EXPECT_FALSE(deque.pop_bottom().has_value());
}

TEST(WorkStealingDequeTest, StealMakesRoomWithoutGrowth) {
    BufferPool<int> pool({{2, 1}});
    WorkStealingDeque<int> deque(pool, 2);
    for (int value = 0; value < 3; ++value) {
        ASSERT_EQ(deque.try_push_bottom(value), PushResult::SUCCESS);
    }

    auto stolen = deque.steal();
    ASSERT_EQ(stolen.state_, StealState::SUCCESS);
    EXPECT_EQ(stolen.value_, 0);
    EXPECT_EQ(deque.try_push_bottom(3), PushResult::SUCCESS);
    EXPECT_EQ(deque.active_array_.load()->log_size(), 2u);
    for (int value : {3, 2, 1}) {
        EXPECT_EQ(deque.pop_bottom(), value);
    }
    EXPECT_FALSE(deque.pop_bottom().has_value());
}

TEST(WorkStealingDequeTest, GrowthCanRetryAfterAnotherDequeReturnsBuffer) {
    BufferPool<int> pool({{2, 1}, {3, 1}});
    WorkStealingDeque<int> deque(pool, 2);
    for (int value = 0; value < 3; ++value) {
        ASSERT_EQ(deque.try_push_bottom(value), PushResult::SUCCESS);
    }

    {
        WorkStealingDeque<int> holder(pool, 3);
        EXPECT_EQ(deque.try_push_bottom(3), PushResult::NO_BUFFER_ACQUIRED);
        EXPECT_EQ(deque.active_array_.load()->log_size(), 2u);
    }

    ASSERT_EQ(deque.try_push_bottom(3), PushResult::SUCCESS);
    EXPECT_EQ(deque.active_array_.load()->log_size(), 3u);
    for (int value = 3; value >= 0; --value) {
        auto popped = deque.pop_bottom();
        ASSERT_TRUE(popped.has_value());
        EXPECT_EQ(*popped, value);
    }
}

} // namespace
