#include "work_stealing_deque.h"

#include <algorithm>
#include <barrier>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

struct Stats {
    std::vector<std::size_t> values;
    std::size_t attempts = 0;
    std::size_t aborts = 0;
    std::size_t empty = 0;
};

// Only the calling thread owns the bottom. Each thief has private statistics;
// validation happens after timing, without adding per-item atomic counters.
bool run(const std::string& mode, std::size_t items, unsigned thieves) {
    WorkStealingDeque<std::size_t> deque(4);
    std::vector<Stats> stats(thieves + 1);
    for (auto& s : stats) s.values.reserve(items);
    const bool prefilled = mode == "prefilled";
    if (prefilled) {
        for (std::size_t i = 0; i < items; ++i) deque.push_bottom(i);
    }

    std::atomic<bool> done{false};
    std::atomic<bool> timed_out{false};
    Clock::time_point start, deadline;
    std::barrier ready(static_cast<std::ptrdiff_t>(thieves + 1), [&]() noexcept {
        start = Clock::now();
        deadline = start + std::chrono::seconds(30);
    });
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < thieves; ++t) {
        workers.emplace_back([&, t] {
            auto& s = stats[t + 1];
            ready.arrive_and_wait();
            for (;;) {
                // Observe producer completion before probing emptiness so a
                // transient empty result cannot cause an early exit.
                const bool finished = done.load(std::memory_order_acquire);
                const auto result = deque.steal();
                ++s.attempts;
                if (result.state_ == StealState::SUCCESS) {
                    s.values.push_back(*result.value_);
                } else if (result.state_ == StealState::ABORT) {
                    ++s.aborts;
                } else {
                    ++s.empty;
                    if (finished) break;
                    std::this_thread::yield();
                }
                if (s.attempts % 1024 == 0 && Clock::now() > deadline) {
                    timed_out.store(true, std::memory_order_relaxed);
                    break;
                }
            }
        });
    }

    ready.arrive_and_wait();
    auto& owner = stats[0];
    std::size_t pushes = 0;
    auto pop = [&] {
        ++owner.attempts;
        if (auto value = deque.pop_bottom()) owner.values.push_back(*value);
        else ++owner.empty;
    };
    if (!prefilled) {
        for (std::size_t i = 0; i < items; ++i) {
            deque.push_bottom(i);
            ++pushes;
            if (mode == "owner") pop();
            // Burst production followed by owner pops exercises resizing as
            // well as competition with thieves at the last element.
            if (mode == "mixed" && (i + 1) % 256 == 0) {
                for (unsigned j = 0; j < 192; ++j) pop();
            }
            if ((i + 1) % 1024 == 0 && Clock::now() > deadline) {
                timed_out.store(true, std::memory_order_relaxed);
                break;
            }
        }
    }
    done.store(true, std::memory_order_release);
    if (mode == "mixed") {
        for (;;) {
            const auto before = owner.values.size();
            pop();
            if (owner.values.size() == before) break;
            if (owner.attempts % 1024 == 0 && Clock::now() > deadline) {
                timed_out.store(true, std::memory_order_relaxed);
                break;
            }
        }
    }
    for (auto& worker : workers) worker.join();
    const double seconds = std::chrono::duration<double>(Clock::now() - start).count();

    std::vector<unsigned char> seen(items, 0);
    std::size_t consumed = 0, duplicates = 0, invalid = 0;
    std::size_t attempts = pushes, aborts = 0, empty = 0;
    for (const auto& s : stats) {
        attempts += s.attempts;
        aborts += s.aborts;
        empty += s.empty;
        consumed += s.values.size();
        for (auto value : s.values) {
            if (value >= items) ++invalid;
            else if (seen[value]) ++duplicates;
            else seen[value] = 1;
        }
    }
    const auto missing = std::count(seen.begin(), seen.end(), 0);
    const bool valid = !timed_out.load() && !missing && !duplicates && !invalid;
    std::cout << std::left << std::setw(11) << mode << " thieves=" << thieves
              << std::fixed << std::setprecision(3)
              << " seconds=" << seconds
              << " Mitems/s=" << consumed / seconds / 1e6
              << " Mcalls/s=" << attempts / seconds / 1e6
              << " ns/item=" << seconds * 1e9 / items
              << " owner=" << owner.values.size()
              << " stolen=" << consumed - owner.values.size()
              << " abort=" << aborts << " empty=" << empty
              << " missing=" << missing << " duplicate=" << duplicates
              << " invalid=" << invalid << " timeout=" << timed_out.load()
              << " " << (valid ? "PASS" : "FAIL") << std::endl;
    return valid;
}
} // namespace

int main(int argc, char** argv) {
    std::size_t items = 100000;
    unsigned max_thieves = std::min(4u, std::max(1u, std::thread::hardware_concurrency()));
    try {
        auto parse = [](const char* arg, unsigned long long limit) {
            const std::string text(arg);
            if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
                throw std::invalid_argument("expected positive integer");
            const auto value = std::stoull(text);
            if (!value || value > limit) throw std::invalid_argument("out of range");
            return value;
        };
        if (argc > 3) throw std::invalid_argument("too many arguments");
        if (argc > 1) items = parse(argv[1], 10000000);
        if (argc > 2) max_thieves = static_cast<unsigned>(parse(argv[2], 64));
    } catch (const std::exception& e) {
        std::cerr << "Usage: " << argv[0] << " [items:1..10000000] [max_thieves:1..64]\n"
                  << e.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "items=" << items << " hardware_threads=" << std::thread::hardware_concurrency()
              << " (timings include bookkeeping and worker completion)\n";
    bool passed = run("owner", items, 0);
    for (unsigned n = 1;; n = std::min(max_thieves, n * 2)) {
        for (const auto* mode : {"prefilled", "streaming", "mixed"})
            passed = run(mode, items, n) && passed;
        if (n == max_thieves) break;
    }
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
