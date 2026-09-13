CXX = g++
CXXFLAGS = -std=c++26 -I/opt/homebrew/include
LDFLAGS = -L/opt/homebrew/lib -lgtest -lgtest_main -pthread
BENCH_ARGS ?= 100000 4
TSAN_ARGS ?= 10000 4
HEADERS = work_stealing_deque.h circular_array.h treiber_stack.h
.PHONY: make clean benchmark benchmark-tsan test test-tsan

make: build/test_deque

build:
	mkdir -p $@

clean:
	rm -rf build

build/test_deque: test_deque.cpp $(HEADERS) | build
	$(CXX) $(CXXFLAGS) $< $(LDFLAGS) -o $@

test: build/test_deque
	./build/test_deque

build/benchmark_deque: benchmark_deque.cpp $(HEADERS) | build
	$(CXX) $(CXXFLAGS) -O3 -DNDEBUG $< -pthread -o $@

benchmark: build/benchmark_deque
	./build/benchmark_deque $(BENCH_ARGS)

build/benchmark_deque_tsan: benchmark_deque.cpp $(HEADERS) | build
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=thread -fno-omit-frame-pointer $< -pthread -o $@

benchmark-tsan: build/benchmark_deque_tsan
	TSAN_OPTIONS=halt_on_error=1 ./build/benchmark_deque_tsan $(TSAN_ARGS)

build/test_deque_tsan: test_deque.cpp $(HEADERS) | build
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=thread -fno-omit-frame-pointer $< $(LDFLAGS) -o $@

test-tsan: build/test_deque_tsan
	TSAN_OPTIONS=halt_on_error=1 ./build/test_deque_tsan
