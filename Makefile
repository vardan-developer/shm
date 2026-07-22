CXX      ?= c++
CXXSTD   := -std=c++20
WARN     := -Wall -Wextra
INCLUDES := -Iinclude
COMMON   := $(CXXSTD) $(WARN) $(INCLUDES) -pthread

BUILD    := build
SRC      := test/test_spsc.cpp
BENCHSRC := test/bench_spsc.cpp
HEADERS  := include/base_buffer.hpp include/spsc_buffer.hpp

BIN      := $(BUILD)/test_spsc
TSAN     := $(BUILD)/test_spsc_tsan
ASAN     := $(BUILD)/test_spsc_asan
BENCH    := $(BUILD)/bench_spsc
MEMCPY   := $(BUILD)/bench_memcpy

.PHONY: all test tsan asan bench bench-memcpy check clean

all: test

# Optimized build + run (the primary correctness run).
test: $(BIN)
	./$(BIN)

$(BIN): $(SRC) $(HEADERS) | $(BUILD)
	$(CXX) $(COMMON) -O2 -g $(SRC) -o $@

# ThreadSanitizer: proves the acquire/release pairing is race-free.
tsan: $(TSAN)
	./$(TSAN)

$(TSAN): $(SRC) $(HEADERS) | $(BUILD)
	$(CXX) $(COMMON) -O1 -g -fsanitize=thread $(SRC) -o $@

# AddressSanitizer: catches any out-of-bounds access in the hand-built buffer.
asan: $(ASAN)
	./$(ASAN)

$(ASAN): $(SRC) $(HEADERS) | $(BUILD)
	$(CXX) $(COMMON) -O1 -g -fsanitize=address,undefined $(SRC) -o $@

# Optimized benchmark for profiling throughput and latency.
bench: $(BENCH)
	./$(BENCH)

$(BENCH): $(BENCHSRC) $(HEADERS) | $(BUILD)
	$(CXX) $(COMMON) -O3 -march=native -g $(BENCHSRC) -o $@

# memcpy latency benchmark (hot vs cold cache).
bench-memcpy: $(MEMCPY)
	./$(MEMCPY)

$(MEMCPY): test/bench_memcpy.cpp | $(BUILD)
	$(CXX) $(COMMON) -O3 -march=native -g test/bench_memcpy.cpp -o $@

# Run everything.
check: test asan tsan

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
