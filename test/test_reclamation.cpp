#define DEQUE_TEST_HOOKS 1
#define private public
#include "work_stealing_deque.h"
#undef private

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <new>
#include <optional>
#include <semaphore>
#include <thread>
#include <vector>

#ifdef DEQUE_TEST_HOOKS

namespace {

struct PauseGate {
    std::binary_semaphore entered{0};
    std::binary_semaphore resume{0};

    static void pause(void* context) {
        auto& gate = *static_cast<PauseGate*>(context);
        gate.entered.release();
        gate.resume.acquire();
    }

    DequeTestHook hook() { return {&pause, this}; }
    bool wait() { return entered.try_acquire_for(std::chrono::seconds(5)); }
    void release() { resume.release(); }
};

// Makes fatal assertions safe while a test worker is paused.
struct PausedWorker {
    PauseGate& gate;
    std::thread& worker;
    bool released = false;

    void resume_and_join() {
        gate.release();
        released = true;
        worker.join();
    }

    ~PausedWorker() {
        if (!released) gate.release();
        if (worker.joinable()) worker.join();
    }
};

enum class ThiefPause { AfterArraySnapshot, AfterSlotRead };

void check_delayed_thief_reuse(ThiefPause pause_at, int reuse_cycles) {
    BufferPool<int> pool({{2, 1}, {3, 1}});
    WorkStealingDeque<int> deque(pool, 2);
    for (int i = 0; i < 4; ++i) {
        ASSERT_EQ(deque.try_push_bottom(i), PushResult::SUCCESS);
    }
    auto* old_array = deque.active_array_.load();
    ASSERT_EQ(old_array->log_size(), 3u);

    PauseGate gate;
    DequeTestHooks hooks{};
    if (pause_at == ThiefPause::AfterArraySnapshot) {
        hooks.after_final_array_snapshot = gate.hook();
    } else {
        hooks.after_speculative_slot_read = gate.hook();
    }
    deque.set_test_hooks(&hooks);
    StealResult<int> delayed{StealState::EMPTY};
    std::thread thief([&] { delayed = deque.steal(); });
    PausedWorker paused{gate, thief};
    ASSERT_TRUE(gate.wait());

    for (int value = 3; value > 0; --value) {
        auto popped = deque.pop_bottom();
        ASSERT_TRUE(popped.has_value());
        EXPECT_EQ(*popped, value);
    }
    EXPECT_EQ(deque.active_array_.load()->log_size(), 2u);

    // Reuse finishes before the old thief resumes. Every iteration reclaims
    // the same permanent record without changing the old pointer's validity.
    for (int cycle = 0; cycle < reuse_cycles; ++cycle) {
        WorkStealingDeque<int> borrower(pool, 3);
        EXPECT_EQ(borrower.active_array_.load(), old_array);
        EXPECT_EQ(borrower.try_push_bottom(1000 + cycle), PushResult::SUCCESS);
        auto item = borrower.pop_bottom();
        ASSERT_TRUE(item.has_value());
        EXPECT_EQ(*item, 1000 + cycle);
    }
    WorkStealingDeque<int> borrower(pool, 3);
    EXPECT_EQ(borrower.active_array_.load(), old_array);
    ASSERT_EQ(borrower.try_push_bottom(9000), PushResult::SUCCESS);

    paused.resume_and_join();
    EXPECT_EQ(delayed.state_, StealState::ABORT);
    EXPECT_FALSE(delayed.value_.has_value());
    auto borrowed = borrower.pop_bottom();
    ASSERT_TRUE(borrowed.has_value());
    EXPECT_EQ(*borrowed, 9000);
    auto remaining = deque.pop_bottom();
    ASSERT_TRUE(remaining.has_value());
    EXPECT_EQ(*remaining, 0);
}

TEST(ReclamationScheduleTest, EnumeratesDelayedThiefReuseSchedules) {
    for (auto pause_at : {ThiefPause::AfterArraySnapshot, ThiefPause::AfterSlotRead}) {
        for (int cycles : {1, 128}) {
            SCOPED_TRACE(pause_at == ThiefPause::AfterArraySnapshot
                             ? "after array snapshot" : "after slot read");
            SCOPED_TRACE(cycles);
            check_delayed_thief_reuse(pause_at, cycles);
        }
    }
}

TEST(ReclamationScheduleTest, FailedShrinkCasRestoresBottomBeforePoolReturn) {
    BufferPool<int> pool({{3, 1}, {4, 1}});
    WorkStealingDeque<int> deque(pool, 3);
    for (int i = 0; i < 8; ++i) {
        ASSERT_EQ(deque.try_push_bottom(i), PushResult::SUCCESS);
    }
    auto* retired = deque.active_array_.load();
    for (int i = 0; i < 4; ++i) {
        auto stolen = deque.steal();
        ASSERT_EQ(stolen.state_, StealState::SUCCESS);
        EXPECT_EQ(*stolen.value_, i);
    }

    PauseGate gate;
    DequeTestHooks hooks{};
    hooks.after_shrink_top_snapshot = gate.hook();
    deque.set_test_hooks(&hooks);
    std::optional<int> popped;
    std::thread owner([&] { popped = deque.pop_bottom(); });
    PausedWorker paused{gate, owner};
    ASSERT_TRUE(gate.wait());

    auto stolen = deque.steal();
    EXPECT_EQ(stolen.state_, StealState::SUCCESS);
    EXPECT_EQ(stolen.value_, 4);
    EXPECT_EQ(pool.try_acquire(4), nullptr);

    paused.resume_and_join();
    ASSERT_TRUE(popped.has_value());
    EXPECT_EQ(*popped, 7);
    EXPECT_EQ(deque.bottom_.load(), 7);
    auto* returned = pool.try_acquire(4);
    ASSERT_EQ(returned, retired);
    pool.release(returned);
    for (int value : {6, 5}) {
        auto remaining = deque.pop_bottom();
        ASSERT_TRUE(remaining.has_value());
        EXPECT_EQ(*remaining, value);
    }
    EXPECT_FALSE(deque.pop_bottom().has_value());
}

TEST(ReclamationScheduleTest, TemporaryEmptyStateUsesModuloCheck) {
    BufferPool<int> pool({{3, 1}, {4, 1}});
    WorkStealingDeque<int> deque(pool, 3);
    for (int i = 0; i < 8; ++i) {
        ASSERT_EQ(deque.try_push_bottom(i), PushResult::SUCCESS);
    }
    for (int i = 0; i < 4; ++i) {
        auto stolen = deque.steal();
        ASSERT_EQ(stolen.state_, StealState::SUCCESS);
        EXPECT_EQ(*stolen.value_, i);
    }

    PauseGate gate;
    DequeTestHooks hooks{};
    hooks.after_shrink_bottom_shift = gate.hook();
    deque.set_test_hooks(&hooks);
    std::optional<int> popped;
    std::thread owner([&] { popped = deque.pop_bottom(); });
    PausedWorker paused{gate, owner};
    ASSERT_TRUE(gate.wait());

    for (int value : {4, 5, 6}) {
        auto stolen = deque.steal();
        EXPECT_EQ(stolen.state_, StealState::SUCCESS);
        EXPECT_EQ(stolen.value_, value);
    }
    EXPECT_EQ(deque.steal().state_, StealState::EMPTY);
    paused.resume_and_join();
    ASSERT_TRUE(popped.has_value());
    EXPECT_EQ(*popped, 7);
    EXPECT_FALSE(deque.pop_bottom().has_value());
}

TEST(ReclamationScheduleTest, OwnerLosesLastItemCasToThief) {
    BufferPool<int> pool({{2, 1}});
    WorkStealingDeque<int> deque(pool, 2);
    ASSERT_EQ(deque.try_push_bottom(42), PushResult::SUCCESS);

    PauseGate thief_gate;
    PauseGate owner_gate;
    DequeTestHooks hooks{};
    hooks.after_final_array_snapshot = thief_gate.hook();
    hooks.before_last_pop_cas = owner_gate.hook();
    deque.set_test_hooks(&hooks);
    StealResult<int> stolen{StealState::EMPTY};
    std::thread thief([&] { stolen = deque.steal(); });
    PausedWorker paused_thief{thief_gate, thief};
    ASSERT_TRUE(thief_gate.wait());

    std::optional<int> popped;
    std::thread owner([&] { popped = deque.pop_bottom(); });
    PausedWorker paused_owner{owner_gate, owner};
    ASSERT_TRUE(owner_gate.wait());

    paused_thief.resume_and_join();
    EXPECT_EQ(stolen.state_, StealState::SUCCESS);
    EXPECT_EQ(stolen.value_, 42);
    paused_owner.resume_and_join();
    EXPECT_FALSE(popped.has_value());
    deque.set_test_hooks(nullptr);

    ASSERT_EQ(deque.try_push_bottom(43), PushResult::SUCCESS);
    popped = deque.pop_bottom();
    ASSERT_TRUE(popped.has_value());
    EXPECT_EQ(*popped, 43);
}

TEST(ReclamationScheduleTest, ShutdownJoinsWorkersBeforeReleasingPoolStorage) {
    BufferPool<int> pool({{2, 1}, {3, 1}, {4, 1}, {5, 1}, {6, 1}});
    std::optional<WorkStealingDeque<int>> deque;
    deque.emplace(pool, 2);

    std::atomic<bool> stop{false};
    std::vector<std::thread> thieves;
    for (int worker = 0; worker < 2; ++worker) {
        thieves.emplace_back([&] {
            while (!stop.load(std::memory_order_acquire)) {
                if (deque->steal().state_ != StealState::SUCCESS) {
                    std::this_thread::yield();
                }
            }
        });
    }
    bool pushed_all = true;
    for (int value = 0; value < 30; ++value) {
        if (deque->try_push_bottom(value) != PushResult::SUCCESS) {
            pushed_all = false;
            break;
        }
    }
    stop.store(true, std::memory_order_release);
    for (auto& thief : thieves) thief.join();
    EXPECT_TRUE(pushed_all);
    while (deque->pop_bottom().has_value()) {}
    deque.reset();

    for (std::size_t log_size = 2; log_size <= 6; ++log_size) {
        auto* available = pool.try_acquire(log_size);
        ASSERT_NE(available, nullptr);
        pool.release(available);
    }
}

TEST(ReclamationScheduleTest, ConcurrentDequesReturnSharedPoolRecords) {
    BufferPool<int> pool({{2, 3}});
    std::atomic<int> completed{0};
    std::atomic<int> errors{0};
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 6; ++worker) {
        workers.emplace_back([&, worker] {
            for (int attempt = 0; attempt < 200; ++attempt) {
                try {
                    WorkStealingDeque<int> deque(pool, 2);
                    const int value = worker * 200 + attempt;
                    if (deque.try_push_bottom(value) != PushResult::SUCCESS) {
                        errors.fetch_add(1);
                    } else {
                        auto popped = deque.pop_bottom();
                        if (!popped || *popped != value) errors.fetch_add(1);
                    }
                    completed.fetch_add(1);
                } catch (const std::bad_alloc&) {
                    std::this_thread::yield();
                }
            }
        });
    }
    for (auto& worker : workers) worker.join();
    EXPECT_GT(completed.load(), 0);
    EXPECT_EQ(errors.load(), 0);

    std::vector<CircularArray<int>*> claimed;
    for (int i = 0; i < 3; ++i) {
        auto* buffer = pool.try_acquire(2);
        ASSERT_NE(buffer, nullptr);
        claimed.push_back(buffer);
    }
    EXPECT_EQ(pool.try_acquire(2), nullptr);
    for (auto* buffer : claimed) pool.release(buffer);
}

} // namespace

#endif // define test hooks
