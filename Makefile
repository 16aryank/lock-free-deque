CXX = g++
CXXFLAGS = -std=c++20 -I/opt/homebrew/include
LDFLAGS = -L/opt/homebrew/lib -lgtest -lgtest_main -pthread
.PHONY: clean

make: test_deque

clean:
	rm -f *.o test_deque

test_deque: test_deque.cpp
	$(CXX) $(CXXFLAGS) $^ $(LDFLAGS) -o $@