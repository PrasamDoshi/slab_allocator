CXX ?= g++
CXXFLAGS ?= -O3 -std=gnu++17 -Wall -Wextra -pedantic
INCLUDES := -Iinclude

.PHONY: all tests bench clean
all: tests bench

build:
	@mkdir -p build

tests: build src/slab.cpp include/slab.hpp tests/test.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) src/slab.cpp tests/test.cpp -o build/tests

bench: build src/slab.cpp include/slab.hpp benchmarks/bench.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) src/slab.cpp benchmarks/bench.cpp -o build/bench

run-tests: tests
	./build/tests

run-bench: bench
	./build/bench

clean:
	rm -rf build
