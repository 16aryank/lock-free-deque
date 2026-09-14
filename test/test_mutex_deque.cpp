#include "mutex/work_stealing_deque.h"
#include <gtest/gtest.h>
#include <deque>

TEST(MutexDequeTest, EmptyAndOppositeEndOrdering) {
    mutex_deque::WorkStealingDeque<int> deque(2);
    EXPECT_FALSE(deque.pop_bottom());
    EXPECT_EQ(deque.steal().state_, StealState::EMPTY);
    deque.push_bottom(10);
    deque.push_bottom(20);
    deque.push_bottom(30);
    EXPECT_EQ(deque.pop_bottom(), 30);
    auto stolen = deque.steal();
    EXPECT_EQ(stolen.state_, StealState::SUCCESS);
    EXPECT_EQ(stolen.value_, 10);
    EXPECT_EQ(deque.pop_bottom(), 20);
    EXPECT_FALSE(deque.pop_bottom());
    EXPECT_EQ(deque.steal().state_, StealState::EMPTY);
}

TEST(MutexDequeTest, WrapGrowShrinkAndReuseMatchModel) {
    mutex_deque::WorkStealingDeque<int> deque(2);
    std::deque<int> model;
    for (int cycle = 0; cycle < 100; ++cycle) {
        for (int i = 0; i < 200; ++i) {
            deque.push_bottom(cycle * 200 + i);
            model.push_back(cycle * 200 + i);
            if (i % 3 == 0) {
                EXPECT_EQ(deque.steal().value_, model.front());
                model.pop_front();
            }
        }
        while (!model.empty()) {
            EXPECT_EQ(deque.pop_bottom(), model.back());
            model.pop_back();
        }
        EXPECT_EQ(deque.steal().state_, StealState::EMPTY);
    }
}
