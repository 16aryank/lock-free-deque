CXX = g++
CXXFLAGS = -std=c++26 -I/opt/homebrew/include
CPPFLAGS += -Isrc
LDFLAGS = -L/opt/homebrew/lib -lgtest -lgtest_main -pthread
BENCH_ARGS ?= 100000 4
TSAN_ARGS ?= 10000 4
PROFILE_ARGS ?= 10000000 4 5 lock-free streaming
PROFILE_OUTPUT ?= build/profile.json.gz
HEADERS = src/work_stealing_deque.h src/circular_array.h src/treiber_stack.h src/steal_result.h src/mutex/work_stealing_deque.h
TEST_SOURCES = test/test_deque.cpp test/test_mutex_deque.cpp
.PHONY: make clean benchmark benchmark-tsan benchmark-profile test test-tsan

make: build/test_deque

build:
	mkdir -p $@

clean:
	rm -rf build

build/test_deque: $(TEST_SOURCES) $(HEADERS) Makefile | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(TEST_SOURCES) $(LDFLAGS) -o $@

test: build/test_deque
	./build/test_deque

build/benchmark_deque: test/benchmark_deque.cpp $(HEADERS) Makefile | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -O3 -DNDEBUG $< -pthread -o $@

benchmark: build/benchmark_deque
	./build/benchmark_deque $(BENCH_ARGS)

build/benchmark_deque_profile: test/benchmark_deque.cpp $(HEADERS) Makefile | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -O3 -g -DNDEBUG -fno-omit-frame-pointer $< -pthread -o $@

benchmark-profile: build/benchmark_deque_profile
	samply record --save-only --unstable-presymbolicate -o $(PROFILE_OUTPUT) -- ./build/benchmark_deque_profile $(PROFILE_ARGS)

build/benchmark_deque_tsan: test/benchmark_deque.cpp $(HEADERS) Makefile | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -O1 -g -fsanitize=thread -fno-omit-frame-pointer $< -pthread -o $@

benchmark-tsan: build/benchmark_deque_tsan
	TSAN_OPTIONS=halt_on_error=1 ./build/benchmark_deque_tsan $(TSAN_ARGS)

build/test_deque_tsan: $(TEST_SOURCES) $(HEADERS) Makefile | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -O1 -g -fsanitize=thread -fno-omit-frame-pointer $(TEST_SOURCES) $(LDFLAGS) -o $@

test-tsan: build/test_deque_tsan
	TSAN_OPTIONS=halt_on_error=1 ./build/test_deque_tsan
