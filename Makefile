CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -Wshadow
LDFLAGS  += -pthread

LIB_SRCS := src/http.cpp src/static_files.cpp src/jobs.cpp src/xml.cpp src/api.cpp src/server.cpp
HEADERS  := $(wildcard src/*.hpp)

.PHONY: all test unit integration sanitize clean

all: server

server: $(LIB_SRCS) src/main.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) $(LIB_SRCS) src/main.cpp -o $@ $(LDFLAGS)

unit_tests: $(LIB_SRCS) tests/unit_tests.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -Isrc $(LIB_SRCS) tests/unit_tests.cpp -o $@ $(LDFLAGS)

unit: unit_tests
	./unit_tests

integration: server
	python3 tests/integration_test.py ./server

test: unit integration

# Build and run everything under AddressSanitizer + UndefinedBehaviorSanitizer.
sanitize: clean
	$(MAKE) test CXXFLAGS="-std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra"

clean:
	rm -f server unit_tests
