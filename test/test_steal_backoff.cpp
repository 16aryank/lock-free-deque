#include "steal_backoff.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace {

struct ScriptedVictim {
    std::vector<StealState> script;
    std::size_t calls = 0;

    StealResult<int> steal() {
        const auto state = calls < script.size() ? script[calls] : StealState::ABORT;
        ++calls;
        if (state == StealState::SUCCESS) return StealResult<int>{42};
        return StealResult<int>{state};
    }
};

TEST(StealBackoffTest, FirstFailureGrowsWindowAndDoesNotPauseAfterLastAttempt) {
    StealBackoff backoff(1, {1, 4, 16});
    ScriptedVictim victim{{StealState::ABORT}};
    std::vector<std::uint32_t> pauses;
    auto pause = [&](std::uint32_t units) { pauses.push_back(units); };

    EXPECT_EQ(steal_with_retry(victim, backoff, pause).state_, StealState::ABORT);
    EXPECT_EQ(victim.calls, 1u);
    EXPECT_TRUE(pauses.empty());
    EXPECT_EQ(backoff.window(), 4u);
}

TEST(StealBackoffTest, GrowthSaturatesAndRetainedWindowContinuesAcrossCalls) {
    StealBackoff backoff(7, {2, 4, 16});
    ScriptedVictim victim;
    std::vector<std::uint32_t> pauses;
    auto pause = [&](std::uint32_t units) { pauses.push_back(units); };
    auto random = [] { return std::uint32_t{0}; };
    auto observe = [](StealState) {};

    EXPECT_EQ(steal_with_retry(victim, backoff, pause, random, observe).state_,
              StealState::ABORT);
    EXPECT_EQ(victim.calls, 2u);
    EXPECT_EQ(backoff.window(), 8u);
    ASSERT_EQ(pauses.size(), 1u);
    EXPECT_EQ(pauses[0], 1u);

    EXPECT_EQ(steal_with_retry(victim, backoff, pause, random, observe).state_,
              StealState::ABORT);
    EXPECT_EQ(victim.calls, 4u);
    EXPECT_EQ(backoff.window(), 16u);
    ASSERT_EQ(pauses.size(), 3u);
    EXPECT_EQ(pauses[1], 1u);
    EXPECT_EQ(pauses[2], 1u);

    EXPECT_EQ(steal_with_retry(victim, backoff, pause, random, observe).state_,
              StealState::ABORT);
    EXPECT_EQ(victim.calls, 6u);
    EXPECT_EQ(backoff.window(), 16u);
    EXPECT_EQ(pauses.size(), 5u);
}

TEST(StealBackoffTest, AttemptsAndPauseUnitsStayWithinConfiguredBounds) {
    constexpr unsigned attempts = 8;
    constexpr std::uint32_t max_window = 64;
    StealBackoff backoff(11, {attempts, 4, max_window});
    ScriptedVictim victim;
    unsigned pauses = 0;
    std::uint32_t total_units = 0;
    auto pause = [&](std::uint32_t units) {
        ++pauses;
        EXPECT_GE(units, 1u);
        EXPECT_LE(units, max_window);
        total_units += units;
    };
    auto random = [] { return ~std::uint32_t{0}; };

    EXPECT_EQ(steal_with_retry(victim, backoff, pause, random,
                               IgnoreStealAttempt{}).state_, StealState::ABORT);
    EXPECT_EQ(victim.calls, attempts);
    EXPECT_EQ(pauses, attempts - 1);
    EXPECT_EQ(total_units, 4u + 8u + 16u + 32u + 64u + 64u + 64u);
    EXPECT_LE(total_units, attempts * max_window);

    const auto before = victim.calls;
    EXPECT_EQ(steal_with_retry(victim, backoff, pause, random,
                               IgnoreStealAttempt{}).state_, StealState::ABORT);
    EXPECT_EQ(victim.calls - before, attempts);
    EXPECT_EQ(total_units, 4u + 8u + 16u + 32u + 64u + 64u + 64u +
                           attempts * max_window);
}

TEST(StealBackoffTest, SuccessAndEmptyResetAndPreserveResult) {
    StealBackoff backoff(23, {4, 4, 16});
    ScriptedVictim victim{{StealState::ABORT, StealState::SUCCESS,
                           StealState::ABORT, StealState::EMPTY}};
    std::vector<std::uint32_t> pauses;
    auto pause = [&](std::uint32_t units) { pauses.push_back(units); };
    auto random = [] { return std::uint32_t{0}; };

    const auto success = steal_with_retry(victim, backoff, pause, random,
                                          IgnoreStealAttempt{});
    EXPECT_EQ(success.state_, StealState::SUCCESS);
    EXPECT_EQ(success.value_, 42);
    EXPECT_EQ(backoff.window(), 0u);
    EXPECT_EQ(victim.calls, 2u);
    EXPECT_EQ(pauses.size(), 1u);

    const auto empty = steal_with_retry(victim, backoff, pause, random,
                                        IgnoreStealAttempt{});
    EXPECT_EQ(empty.state_, StealState::EMPTY);
    EXPECT_FALSE(empty.value_.has_value());
    EXPECT_EQ(backoff.window(), 0u);
    EXPECT_EQ(victim.calls, 4u);
    EXPECT_EQ(pauses.size(), 2u);
}

TEST(StealBackoffTest, VictimSwitchAndNewSearchResetOnlyTheWindow) {
    StealBackoff backoff(31, {1, 4, 16});
    StealBackoff control(31, {1, 4, 16});
    ScriptedVictim first;
    ScriptedVictim second;
    std::vector<std::uint32_t> pauses;
    auto pause = [&](std::uint32_t units) { pauses.push_back(units); };

    EXPECT_EQ(steal_with_retry(first, backoff, pause).state_, StealState::ABORT);
    EXPECT_EQ(backoff.window(), 4u);
    EXPECT_EQ(steal_with_retry(second, backoff, pause).state_, StealState::ABORT);
    EXPECT_TRUE(pauses.empty());
    EXPECT_EQ(backoff.window(), 4u);
    EXPECT_EQ(steal_with_retry(second, backoff, pause).state_, StealState::ABORT);
    ASSERT_EQ(pauses.size(), 1u);
    EXPECT_EQ(backoff.window(), 8u);

    backoff.begin_search();
    EXPECT_EQ(steal_with_retry(second, backoff, pause).state_, StealState::ABORT);
    EXPECT_EQ(pauses.size(), 1u);
    EXPECT_EQ(backoff.window(), 4u);
    // The reset never rewinds the PRNG sequence.
    EXPECT_NE(backoff.next_random(), control.next_random());
}

TEST(StealBackoffTest, WorkersKeepIndependentStateAndObserveRawAttempts) {
    StealBackoff first(1, {2, 4, 16});
    StealBackoff second(2, {2, 4, 16});
    ScriptedVictim victim{{StealState::ABORT, StealState::ABORT,
                           StealState::SUCCESS}};
    std::vector<StealState> observed;
    auto pause = [](std::uint32_t) {};
    auto observe = [&](StealState state) { observed.push_back(state); };
    auto random = [] { return std::uint32_t{0}; };

    EXPECT_EQ(steal_with_retry(victim, first, pause, random, observe).state_,
              StealState::ABORT);
    EXPECT_EQ(first.window(), 8u);
    EXPECT_EQ(second.window(), 0u);
    EXPECT_EQ(steal_with_retry(victim, second, pause, random, observe).state_,
              StealState::SUCCESS);
    EXPECT_EQ(first.window(), 8u);
    EXPECT_EQ(second.window(), 0u);
    EXPECT_EQ((std::vector<StealState>{StealState::ABORT, StealState::ABORT,
                                       StealState::SUCCESS}), observed);
}

TEST(StealBackoffTest, RejectsInvalidConfiguration) {
    EXPECT_NO_THROW((StealBackoff(1, {4, 4,
        StealBackoffConfig::max_supported_window})));
    EXPECT_THROW((StealBackoff(0)), std::invalid_argument);
    EXPECT_THROW((StealBackoff(1, {0, 4, 16})), std::invalid_argument);
    EXPECT_THROW((StealBackoff(1, {4, 3, 16})), std::invalid_argument);
    EXPECT_THROW((StealBackoff(1, {4, 4,
        StealBackoffConfig::max_supported_window * 2})), std::invalid_argument);
    EXPECT_THROW((StealBackoff(1, {4, 16, 8})), std::invalid_argument);
}

} // namespace
