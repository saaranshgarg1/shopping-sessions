CXX ?= g++
BUILD_DIR ?= /tmp/shopping-native-build
CXXFLAGS ?= -O3 -DNDEBUG -std=c++20 -pthread -Wall -Wextra -Wno-missing-field-initializers

PYTHON ?= python3

.PHONY: all test parquet-test clean
all: $(BUILD_DIR)/shopping-sessions

$(BUILD_DIR):
	mkdir -p "$@"

$(BUILD_DIR)/shopping-sessions: src/main.cpp src/core.hpp src/parquet.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -Isrc src/main.cpp -o "$@"

$(BUILD_DIR)/core-test: tests/core_test.cpp src/core.hpp | $(BUILD_DIR)
	$(CXX) -O2 -std=c++20 -pthread -Wall -Wextra -Isrc tests/core_test.cpp -o "$@"

test: $(BUILD_DIR)/core-test
	$(BUILD_DIR)/core-test

$(BUILD_DIR)/parquet-probe: tests/parquet_probe.cpp src/parquet.hpp | $(BUILD_DIR)
	$(CXX) -O2 -std=c++20 -Wall -Wextra -Isrc tests/parquet_probe.cpp -o "$@"

parquet-test: $(BUILD_DIR)/parquet-probe
	@set -e; out=$$(mktemp -d "$(BUILD_DIR)/parquet-test.XXXXXX"); \
	"$(BUILD_DIR)/parquet-probe" "$$out"; \
	$(PYTHON) -I tests/check_parquet.py "$$out"

clean:
	rm -f "$(BUILD_DIR)/shopping-sessions" "$(BUILD_DIR)/core-test" "$(BUILD_DIR)/parquet-probe"
