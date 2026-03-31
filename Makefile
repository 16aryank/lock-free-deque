CC := clang++

CFLAGS += -g -Wall -std=c++23

# Generate the names of the thread library's object files
THREAD_OBJS := ${THREAD_SOURCES:.cpp=.o}

all: libthread.o testsample test_*

# Compile the thread library and tag this compilation
libthread.o: ${THREAD_OBJS}
	@ THREAD_HEADERS=$$(${CC} ${CFLAGS} -E -H ${THREAD_SOURCES} 2>&1 > /dev/null | grep '^\.\.* [^/]' | sed 's/^\.* //' | sort -u) ; \
	./autotag.sh ${THREAD_SOURCES} $${THREAD_HEADERS} push
	ld -r -o $@ ${THREAD_OBJS}

# Generic rules for compiling a source file to an object file
%.o: %.cpp
	${CC} ${CFLAGS} -c $<

clean:
	rm -f ${THREAD_OBJS} libthread.o testsample
