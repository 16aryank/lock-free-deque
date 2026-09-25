#include "work_stealing_deque.h"

#include <gtest/gtest.h>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <optional>

namespace {
thread_local bool count_allocations = false;
thread_local std::size_t allocation_count = 0;
}

// Replacing global new counts allocations made inside deque operations.
// The default new[] forwards to this function, so array allocations count too.
void* operator new(std::size_t size) {
    if (count_allocations) ++allocation_count;
    if (void* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc{};
}

// New allocates with malloc, so delete needs to call free
void operator delete(void* memory) noexcept { std::free(memory); }

namespace {

TEST(ReclamationAllocationTest, ArrayNewUsesAllocationCounter) {
    allocation_count = 0;
    count_allocations = true;
    void* memory = ::operator new[](sizeof(int));
    count_allocations = false;
    ::operator delete[](memory);
    EXPECT_EQ(allocation_count, 1u);
}

TEST(ReclamationAllocationTest, OperationsDoNotAllocateAfterSetup) {
    BufferPool<int> pool({{2, 1}, {3, 1}});
    WorkStealingDeque<int> deque(pool, 2);
    std::optional<WorkStealingDeque<int>> holder;
    holder.emplace(pool, 3);

    allocation_count = 0;
    count_allocations = true;
    const auto first = deque.try_push_bottom(0);
    const auto second = deque.try_push_bottom(1);
    const auto third = deque.try_push_bottom(2);
    const auto exhausted = deque.try_push_bottom(3);
    holder.reset();
    const auto grown = deque.try_push_bottom(3);
    const auto stolen = deque.steal();
    const auto pop_three = deque.pop_bottom();
    const auto pop_two = deque.pop_bottom();
    const auto pop_one = deque.pop_bottom();
    auto* recycled = pool.try_acquire(3);
    if (recycled) pool.release(recycled);
    count_allocations = false;
    const auto observed_allocations = allocation_count;

    EXPECT_EQ(observed_allocations, 0u);
    EXPECT_EQ(first, PushResult::SUCCESS);
    EXPECT_EQ(second, PushResult::SUCCESS);
    EXPECT_EQ(third, PushResult::SUCCESS);
    EXPECT_EQ(exhausted, PushResult::NO_BUFFER_ACQUIRED);
    EXPECT_EQ(grown, PushResult::SUCCESS);
    EXPECT_EQ(stolen.state_, StealState::SUCCESS);
    EXPECT_EQ(stolen.value_, 0);
    EXPECT_EQ(pop_three, 3);
    EXPECT_EQ(pop_two, 2);
    EXPECT_EQ(pop_one, 1);
    EXPECT_NE(recycled, nullptr);
}

} // namespace
