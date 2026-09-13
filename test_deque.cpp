#define private public
#include "work_stealing_deque.h"
#undef private

#include <gtest/gtest.h>
#include <atomic>
#include <deque>
#include <thread>
#include <string>

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

constexpr std::size_t kDefaultLogSize = 4;

TEST(WorkStealingDequeTest, StealReturnsEmptySuccessAndAbort) {
    WorkStealingDeque<int> deque{kDefaultLogSize};

    // Empty case.
    auto empty = deque.steal();
    EXPECT_EQ(empty.state_, StealState::EMPTY);

    // Success case.
    deque.push_bottom(42);
    auto success = deque.steal();
    EXPECT_EQ(success.state_, StealState::SUCCESS);
    ASSERT_TRUE(success.value_.has_value());
    EXPECT_EQ(success.value_.value(), 42);

    // Abort case: race two stealers on a single element.
    bool saw_abort = false;
    bool saw_success = false;
    constexpr int kAttempts = 10000;
    for (int i = 0; i < kAttempts && !saw_abort; ++i) {
        WorkStealingDeque<int> d(2);
        d.push_bottom(i);

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
    WorkStealingDeque<int> deque(4); // size 16
    std::deque<int> model;

    const int iterations = 2000;
    for (int i = 0; i < iterations; ++i) {
        // Grow to near full.
        while (model.size() < 15) {
            deque.push_bottom(i);
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
    WorkStealingDeque<int> deque(2); // size 4

    // Force multiple grows.
    for (int i = 0; i < 200; ++i) {
        deque.push_bottom(i);
    }

    auto before = std::atomic_load_explicit(&deque.active_array_, std::memory_order_acquire);
    auto before_log = before->log_size();
    ASSERT_GE(before_log, 4u);

    // Pop until size is tiny to trigger multi-shrink.
    while (true) {
        auto v = deque.pop_bottom();
        if (!v.has_value()) {
            break;
        }
        auto cur = std::atomic_load_explicit(&deque.active_array_, std::memory_order_acquire);
        if (cur->log_size() + 1 < before_log) {
            break;
        }
    }

    auto after = std::atomic_load_explicit(&deque.active_array_, std::memory_order_acquire);
    EXPECT_LE(after->log_size() + 1, before_log);
}

TEST(WorkStealingDequeTest, PoolReusesBuffersAcrossDeques) {
    constexpr std::size_t log_size = 3;
    CircularArray<int>* freed = nullptr;
    {
        WorkStealingDeque<int> d(log_size);
        freed = std::atomic_load_explicit(&d.active_array_, std::memory_order_acquire).get();
    }

    WorkStealingDeque<int> d2(log_size);
    auto reused = std::atomic_load_explicit(&d2.active_array_, std::memory_order_acquire).get();
    EXPECT_EQ(reused, freed);
}

} // namespace
