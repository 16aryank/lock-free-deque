#include "work_stealing_deque.h"
#include "mutex/work_stealing_deque.h"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using Value = std::size_t;
using Clock = std::chrono::steady_clock;

struct Result {
    double seconds;
    std::size_t owner;
    std::size_t stolen;
    bool valid;
};

bool push(WorkStealingDeque<Value>& deque, Value value) {
    return deque.try_push_bottom(value) == PushResult::SUCCESS;
}

bool push(mutex_deque::WorkStealingDeque<Value>& deque, Value value) {
    deque.push_bottom(value);
    return true;
}

bool contains_each_value(const std::vector<Value>& owner,
                         const std::vector<std::vector<Value>>& stolen,
                         std::size_t items) {
    std::vector<unsigned char> seen(items, 0);
    auto mark = [&](Value value) {
        if (value >= items || seen[value]) return false;
        seen[value] = 1;
        return true;
    };
    for (Value value : owner) if (!mark(value)) return false;
    for (const auto& worker : stolen)
        for (Value value : worker) if (!mark(value)) return false;
    return std::all_of(seen.begin(), seen.end(), [](unsigned char value) { return value == 1; });
}

// The owner pushes while thieves steal. After they finish, the owner drains
// anything left at the bottom. Setup and validation are outside the timer.
template <class Deque>
Result run(Deque& deque, std::size_t items, unsigned thieves) {
    std::vector<std::vector<Value>> stolen(thieves);
    for (auto& values : stolen) values.reserve(items / thieves + 1);
    std::vector<Value> owner;
    owner.reserve(items);
    std::vector<std::thread> workers;
    workers.reserve(thieves);
    std::atomic<bool> done{false};
    Clock::time_point start;
    std::barrier ready(static_cast<std::ptrdiff_t>(thieves + 1), [&]() noexcept {
        start = Clock::now();
    });

    for (unsigned t = 0; t < thieves; ++t) {
        workers.emplace_back([&, t] {
            ready.arrive_and_wait();
            for (;;) {
                const bool finished = done.load(std::memory_order_acquire);
                const auto result = deque.steal();
                if (result.state_ == StealState::SUCCESS) {
                    stolen[t].push_back(*result.value_);
                } else if (result.state_ == StealState::EMPTY && finished) {
                    break;
                } else if (result.state_ == StealState::EMPTY) {
                    std::this_thread::yield();
                }
            }
        });
    }

    ready.arrive_and_wait();
    bool pushed_all = true;
    for (Value value = 0; value < items; ++value) {
        if (!push(deque, value)) {
            pushed_all = false;
            break;
        }
    }
    done.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();
    while (auto value = deque.pop_bottom()) owner.push_back(*value);
    const double seconds = std::chrono::duration<double>(Clock::now() - start).count();

    std::size_t stolen_count = 0;
    for (const auto& values : stolen) stolen_count += values.size();
    const bool valid = pushed_all && contains_each_value(owner, stolen, items);
    return {seconds, owner.size(), stolen_count, valid};
}

std::unique_ptr<BufferPool<Value>> make_pool(std::size_t items) {
    std::vector<BufferPool<Value>::SizeClassConfig> sizes;
    for (std::size_t log_size = 4;; ++log_size) {
        sizes.push_back({log_size, 1});
        if ((Value{1} << log_size) > items) break;
    }
    return std::make_unique<BufferPool<Value>>(std::move(sizes));
}

void print_result(std::string_view name, const Result& result, std::size_t items) {
    std::cout << name << " seconds=" << std::fixed << std::setprecision(6)
              << result.seconds << " Mitems/s=" << std::setprecision(2)
              << items / result.seconds / 1e6
              << " owner=" << result.owner << " stolen=" << result.stolen
              << ' ' << (result.valid ? "PASS" : "FAIL") << '\n';
}
} // namespace

int main(int argc, char** argv) {
    if (argc > 4 || (argc == 4 && std::string_view(argv[3]) != "compare")) {
        std::cerr << "Usage: " << argv[0] << " [items] [thieves] [compare]\n";
        return EXIT_FAILURE;
    }

    const std::size_t items = argc > 1 ? std::stoull(argv[1]) : 100000;
    const std::size_t thief_count = argc > 2 ? std::stoull(argv[2]) : 4;
    if (items == 0 || items > 10000000 || thief_count > 64) {
        std::cerr << "items must be 1..10000000; thieves must be 0..64\n";
        return EXIT_FAILURE;
    }
    const unsigned thieves = static_cast<unsigned>(thief_count);

    std::cout << "items=" << items << " thieves=" << thieves << '\n';
    auto pool = make_pool(items);
    WorkStealingDeque<Value> deque(*pool, 4);
    const Result lock_free = run(deque, items, thieves);
    print_result("lock-free", lock_free, items);
    if (argc < 4) return lock_free.valid ? EXIT_SUCCESS : EXIT_FAILURE;

    mutex_deque::WorkStealingDeque<Value> mutex_deque(4);
    const Result mutex = run(mutex_deque, items, thieves);
    print_result("mutex", mutex, items);
    if (lock_free.valid && mutex.valid)
        std::cout << "mutex/lock-free time=" << mutex.seconds / lock_free.seconds << "x\n";
    return lock_free.valid && mutex.valid ? EXIT_SUCCESS : EXIT_FAILURE;
}
