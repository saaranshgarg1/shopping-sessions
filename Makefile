CXX ?= g++
BUILD_DIR ?= build
CXXFLAGS ?= -O3 -flto -mtune=cascadelake -fno-semantic-interposition -DNDEBUG -std=c++20 -pthread -Wall -Wextra -Wno-missing-field-initializers

PYTHON ?= python3

.PHONY: all test parquet-test clean
all: $(BUILD_DIR)/shopping-sessions

$(BUILD_DIR)/calibrate: tests/calibrate.cpp src/main.cpp src/core.hpp src/parquet.hpp src/fast.hpp src/v5.hpp src/reduce.hpp src/scanner.hpp src/packed_ids.hpp src/timing.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -Isrc tests/calibrate.cpp -o "$@"

$(BUILD_DIR):
	mkdir -p "$@"

$(BUILD_DIR)/shopping-sessions: src/main.cpp src/core.hpp src/parquet.hpp src/fast.hpp src/v5.hpp src/reduce.hpp src/scanner.hpp src/packed_ids.hpp src/timing.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -Isrc src/main.cpp -o "$@"

$(BUILD_DIR)/core-test: tests/core_test.cpp src/core.hpp | $(BUILD_DIR)
	$(CXX) -O2 -std=c++20 -pthread -Wall -Wextra -Isrc tests/core_test.cpp -o "$@"

$(BUILD_DIR)/perf-test: tests/perf_test.cpp src/main.cpp src/core.hpp src/parquet.hpp src/fast.hpp src/v5.hpp src/reduce.hpp src/scanner.hpp src/packed_ids.hpp src/timing.hpp | $(BUILD_DIR)
	$(CXX) -O2 -std=c++20 -pthread -Wall -Wextra -Wno-missing-field-initializers -Isrc tests/perf_test.cpp -o "$@"

$(BUILD_DIR)/fast-test: tests/fast_test.cpp src/main.cpp src/core.hpp src/parquet.hpp src/fast.hpp src/v5.hpp src/reduce.hpp src/scanner.hpp src/packed_ids.hpp src/timing.hpp | $(BUILD_DIR)
	$(CXX) -O2 -std=c++20 -pthread -Wall -Wextra -Wno-missing-field-initializers -Isrc tests/fast_test.cpp -o "$@"

$(BUILD_DIR)/reduce-test: tests/reduce_test.cpp src/core.hpp src/reduce.hpp | $(BUILD_DIR)
	$(CXX) -O2 -std=c++20 -pthread -Wall -Wextra -Isrc tests/reduce_test.cpp -o "$@"

$(BUILD_DIR)/v5-test: tests/v5_test.cpp src/main.cpp src/core.hpp src/parquet.hpp src/fast.hpp src/v5.hpp src/reduce.hpp src/scanner.hpp src/packed_ids.hpp src/timing.hpp | $(BUILD_DIR)
	$(CXX) -O2 -std=c++20 -pthread -Wall -Wextra -Wno-missing-field-initializers -Isrc tests/v5_test.cpp -o "$@"

$(BUILD_DIR)/scanner-test: tests/scanner_test.cpp src/scanner.hpp src/core.hpp | $(BUILD_DIR)
	$(CXX) -O2 -std=c++20 -Wall -Wextra -Isrc tests/scanner_test.cpp -o "$@"

$(BUILD_DIR)/packed-test: tests/packed_test.cpp src/packed_ids.hpp src/core.hpp | $(BUILD_DIR)
	$(CXX) -O2 -std=c++20 -Wall -Wextra -Isrc tests/packed_test.cpp -o "$@"

test: $(BUILD_DIR)/core-test $(BUILD_DIR)/perf-test $(BUILD_DIR)/fast-test $(BUILD_DIR)/reduce-test $(BUILD_DIR)/v5-test $(BUILD_DIR)/scanner-test $(BUILD_DIR)/packed-test
	$(BUILD_DIR)/core-test
	$(BUILD_DIR)/perf-test
	$(BUILD_DIR)/fast-test
	$(BUILD_DIR)/reduce-test
	$(BUILD_DIR)/v5-test
	$(BUILD_DIR)/scanner-test
	$(BUILD_DIR)/packed-test

$(BUILD_DIR)/parquet-probe: tests/parquet_probe.cpp src/parquet.hpp | $(BUILD_DIR)
	$(CXX) -O2 -std=c++20 -Wall -Wextra -Isrc tests/parquet_probe.cpp -o "$@"

parquet-test: $(BUILD_DIR)/parquet-probe
	@set -e; out=$$(mktemp -d "$(BUILD_DIR)/parquet-test.XXXXXX"); \
	"$(BUILD_DIR)/parquet-probe" "$$out"; \
	$(PYTHON) -I tests/check_parquet.py "$$out"

clean:
	rm -f "$(BUILD_DIR)/shopping-sessions" "$(BUILD_DIR)/core-test" "$(BUILD_DIR)/perf-test" "$(BUILD_DIR)/fast-test" "$(BUILD_DIR)/reduce-test" "$(BUILD_DIR)/v5-test" "$(BUILD_DIR)/calibrate" "$(BUILD_DIR)/parquet-probe"
