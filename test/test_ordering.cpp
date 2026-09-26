#include "work_stealing_deque.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <thread>
#include <vector>

namespace {

TEST(OrderingTest, TwoItemsOwnerPopAndTwoStealsAreExclusive) {
    BufferPool<int> pool({{2, 1}});
    for (int round = 0; round < 256; ++round) {
        WorkStealingDeque<int> deque(pool, 2);
        ASSERT_EQ(deque.try_push_bottom(2 * round), PushResult::SUCCESS);
        ASSERT_EQ(deque.try_push_bottom(2 * round + 1), PushResult::SUCCESS);

        std::barrier start(3);
        std::array<StealResult<int>, 2> steals{
            StealResult<int>{StealState::EMPTY}, StealResult<int>{StealState::EMPTY}};
        std::thread first([&] { start.arrive_and_wait(); steals[0] = deque.steal(); });
        std::thread second([&] { start.arrive_and_wait(); steals[1] = deque.steal(); });
        start.arrive_and_wait();

        std::vector<int> values;
        while (auto value = deque.pop_bottom()) values.push_back(*value);
        first.join();
        second.join();
        for (const auto& steal : steals) {
            if (steal.state_ == StealState::SUCCESS) {
                ASSERT_TRUE(steal.value_.has_value());
                values.push_back(*steal.value_);
            } else {
                EXPECT_FALSE(steal.value_.has_value());
            }
        }
        EXPECT_EQ(values.size(), 2u);
        EXPECT_EQ(std::count(values.begin(), values.end(), 2 * round), 1);
        EXPECT_EQ(std::count(values.begin(), values.end(), 2 * round + 1), 1);
    }
}

struct Payload {
    int identity;
    int checksum;
};

static_assert(LockFreeAtomicValue<Payload*>);

TEST(OrderingTest, PointerPayloadFieldsArePublishedBeforeSteal) {
    constexpr int count = 1024;
    BufferPool<Payload*> pool({{2, 1}, {3, 1}, {4, 1}, {5, 1}, {6, 1},
                               {7, 1}, {8, 1}, {9, 1}, {10, 1}, {11, 1}});
    WorkStealingDeque<Payload*> deque(pool, 2);
    std::vector<Payload> payloads(count);
    std::array<std::vector<int>, 2> stolen;
    std::atomic<bool> done{false};
    std::atomic<int> bad_payloads{0};
    std::barrier start(3);
    std::array<std::thread, 2> thieves;

    for (int worker = 0; worker < 2; ++worker) {
        thieves[worker] = std::thread([&, worker] {
            start.arrive_and_wait();
            for (;;) {
                auto result = deque.steal();
                if (result.state_ == StealState::SUCCESS) {
                    if (!result.value_) {
                        bad_payloads.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                    auto* value = *result.value_;
                    int identity = 0;
                    while (identity < count && value != &payloads[identity]) ++identity;
                    if (identity == count) {
                        bad_payloads.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                    if (value->identity != identity ||
                        value->checksum != identity * 17 + 3) {
                        bad_payloads.fetch_add(1, std::memory_order_relaxed);
                    }
                    stolen[worker].push_back(identity);
                } else if (result.state_ == StealState::EMPTY &&
                           done.load(std::memory_order_acquire)) {
                    break;
                }
            }
        });
    }

    start.arrive_and_wait();
    bool pushed_all = true;
    for (int identity = 0; identity < count; ++identity) {
        payloads[identity] = {identity, identity * 17 + 3};
        if (deque.try_push_bottom(&payloads[identity]) != PushResult::SUCCESS) {
            pushed_all = false;
            break;
        }
    }
    done.store(true, std::memory_order_release);
    for (auto& thief : thieves) thief.join();

    std::vector<int> seen(count, 0);
    for (const auto& history : stolen)
        for (int identity : history) ++seen[identity];
    while (auto result = deque.pop_bottom()) {
        int identity = 0;
        while (identity < count && *result != &payloads[identity]) ++identity;
        if (identity == count) {
            ++bad_payloads;
            continue;
        }
        ++seen[identity];
    }
    EXPECT_TRUE(pushed_all);
    EXPECT_EQ(bad_payloads.load(), 0);
    for (int uses : seen) EXPECT_EQ(uses, 1);
}

} // namespace
