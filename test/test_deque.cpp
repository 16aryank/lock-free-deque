#define private public
#include "work_stealing_deque.h"
#undef private

#include <gtest/gtest.h>
#include <atomic>
#include <deque>
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
    for (int i = 0; i < iterations; ++i) {
        // Grow to near full.
        while (model.size() < 15) {
            ASSERT_EQ(deque.try_push_bottom(i), PushResult::SUCCESS);
            model.push_back(i);
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

    auto* before = deque.active_array_.load(std::memory_order_seq_cst);
    auto before_log = before->log_size();
    ASSERT_GE(before_log, 4u);

    // Pop until size is tiny to trigger multi-shrink.
    while (true) {
        auto v = deque.pop_bottom();
        if (!v.has_value()) {
            break;
        }
        auto* cur = deque.active_array_.load(std::memory_order_seq_cst);
        if (cur->log_size() + 1 < before_log) {
            break;
        }
    }

    auto* after = deque.active_array_.load(std::memory_order_seq_cst);
    EXPECT_LE(after->log_size() + 1, before_log);
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

TEST(WorkStealingDequeTest, RetainedRawPointerChainsReturnAtShutdown) {
    BufferPool<int> pool({{2, 1}, {3, 1}, {4, 1}, {5, 1}});
    {
        WorkStealingDeque<int> deque(pool, 2);
        for (int i = 0; i < 20; ++i) {
            ASSERT_EQ(deque.try_push_bottom(i), PushResult::SUCCESS);
        }
        EXPECT_EQ(deque.active_array_.load()->log_size(), 5u);

        for (int i = 19; i >= 0; --i) {
            auto value = deque.pop_bottom();
            ASSERT_TRUE(value.has_value());
            EXPECT_EQ(*value, i);
        }
        EXPECT_NE(deque.retired_, nullptr);
        for (std::size_t log_size = 2; log_size <= 5; ++log_size) {
            EXPECT_EQ(pool.try_acquire(log_size), nullptr);
        }
    }

    for (std::size_t log_size = 2; log_size <= 5; ++log_size) {
        auto* buffer = pool.try_acquire(log_size);
        ASSERT_NE(buffer, nullptr);
        EXPECT_EQ(buffer->log_size(), log_size);
        EXPECT_EQ(buffer->get_prev(), nullptr);
        pool.release(buffer);
    }
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
