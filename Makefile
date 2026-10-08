CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -pthread
SRCS     := src/main.cpp src/http.cpp
HDRS     := $(wildcard src/*.hpp)

.PHONY: all tsan test clean

all: server

server: $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) $(SRCS) -o $@

# ThreadSanitizer build: detects data races at runtime.
server_tsan: $(SRCS) $(HDRS)
	$(CXX) -std=c++17 -g -O1 -pthread -fsanitize=thread $(SRCS) -o $@

tsan: server_tsan

test: server
	bash tests/run_tests.sh ./server

clean:
	rm -f server server_tsan
