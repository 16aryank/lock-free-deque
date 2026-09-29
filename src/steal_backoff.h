#pragma once

#include "steal_result.h"

#include <cstdint>
#include <stdexcept>
#include <algorithm>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

struct StealBackoffConfig {
    static constexpr std::uint32_t max_supported_window = std::numeric_limits<uint16_t>::max();
    unsigned max_attempts = 4;
    std::uint32_t initial_window = 4;
    std::uint32_t max_window = 256;
};

class StealBackoff {
public:
    explicit StealBackoff(std::uint32_t seed,
                          StealBackoffConfig config = {})
        : config_(config), random_state_(seed) {
        if (seed == 0 || !validate_steal_config(config)) {
            throw std::invalid_argument("invalid steal backoff configuration");
        }
    }

    [[nodiscard]] const StealBackoffConfig& config() const noexcept { return config_; }
    [[nodiscard]] std::uint32_t window() const noexcept { return window_; }

    void reset() noexcept { window_ = 0; }

    // Explicitly call this when a search ends, including when the next deque
    // could reuse the same address. It does not rewind the random sequence.
    void begin_search() noexcept {
        reset();
        victim_ = nullptr;
    }

    void select_victim(const void* victim) noexcept {
        if (victim_ != victim) {
            reset();
            victim_ = victim;
        }
    }

    void record_abort() noexcept {
        if (window_ == 0) {
            window_ = config_.initial_window;
        } else {
            window_ = std::min(window_ * 2, config_.max_window);
        }
    }

    [[nodiscard]] std::uint32_t next_random() noexcept {
        // Xorshift32 advances this worker's private, nonzero state. The
        // sequence is deterministic for a seed, so jitter needs no shared
        // state, synchronization, or clock read. It is not for cryptography.
        random_state_ ^= random_state_ << 13;
        random_state_ ^= random_state_ >> 17;
        random_state_ ^= random_state_ << 5;
        return random_state_;
    }

private:
    StealBackoffConfig config_;
    std::uint32_t random_state_;
    std::uint32_t window_ = 0;
    const void* victim_ = nullptr;

    const bool validate_steal_config(const StealBackoffConfig& config) noexcept {
        auto is_power_of_two = [](auto n) {
            return (n > 0) && ((n & (n - 1)) == 0);
        };
        return config.max_attempts > 0 && config.max_attempts <= 64 // max attempts
            && is_power_of_two(config.initial_window) && config.initial_window <= config.max_window // initial window
            && is_power_of_two(config.max_window) && config.max_attempts <= StealBackoffConfig::max_supported_window; // maxwindow
    }
};

struct CpuPause {
    // One unit executes one architecture-specific spin instruction. Units
    // bound work, not elapsed time; this never sleeps or yields the thread.
    void operator()(std::uint32_t units) const noexcept {
        for (std::uint32_t i = 0; i < units; ++i) {
#if defined(__x86_64__) || defined(__i386__)
            // Hint that this thread is in a short spin loop.
            _mm_pause();
#elif defined(__aarch64__)
            // ISB supplies a bounded ARM64 delay; it is not a timer.
            __asm__ __volatile__("isb");
#else
            // A compiler barrier keeps the loop on targets without a backend.
            __asm__ __volatile__("" ::: "memory");
#endif
        } // for..
    } // operator()
};

struct IgnoreStealAttempt {
    void operator()(StealState) const noexcept {}
};

// Each call makes at most max_attempts fresh deque attempts. The caller can
// inspect ABORT, do other work, and retry later with the same worker context.
template <class Deque, class Pause, class Random, class Observe>
auto steal_with_retry(Deque& victim, StealBackoff& backoff,
                      Pause&& pause, Random&& random, Observe&& observe)
    -> decltype(victim.steal()) {
    backoff.select_victim(&victim);
    for (unsigned attempt = 0; attempt < backoff.config().max_attempts; ++attempt) {
        if (backoff.window() != 0) {
            // The window is a power of two, so masking gives [1, window].
            pause(1 + (random() & (backoff.window() - 1)));
        }

        auto result = victim.steal();
        observe(result.state_);
        if (result.state_ != StealState::ABORT) {
            backoff.reset();
            return result;
        }
        backoff.record_abort();
    }
    return decltype(victim.steal()){StealState::ABORT};
}

template <class Deque, class Pause>
auto steal_with_retry(Deque& victim, StealBackoff& backoff, Pause&& pause)
    -> decltype(victim.steal()) {
    return steal_with_retry(victim, backoff, pause,
                            [&] { return backoff.next_random(); },
                            IgnoreStealAttempt{});
}

template <class Deque>
auto steal_with_retry(Deque& victim, StealBackoff& backoff)
    -> decltype(victim.steal()) {
    return steal_with_retry(victim, backoff, CpuPause{});
}
