# GSSK Build System
CC      ?= gcc
CFLAGS  = -Wall -Wextra -Werror -std=c99 -Iinclude -fPIC
LDFLAGS = -lm

# Optimization levels (Use 'make DEBUG=1' for debugging)
ifeq ($(DEBUG), 1)
	CFLAGS += -g -O0 -DDEBUG
else
	CFLAGS += -O3 -march=native
endif

# Directories
SRC_DIR = src
INC_DIR = include
BIN_DIR = bin
LIB_DIR = lib
DIST_DIR = dist
TEST_DIR = tests

# Files
SOURCES = $(SRC_DIR)/gssk.c $(SRC_DIR)/advanced.c $(SRC_DIR)/cJSON.c
OBJECTS = $(LIB_DIR)/gssk.o $(LIB_DIR)/advanced.o $(LIB_DIR)/cJSON.o
TARGET_LIB = $(LIB_DIR)/libgssk.a
TARGET_CLI = $(BIN_DIR)/gssk
TARGET_COMPARE = $(BIN_DIR)/csv_compare

.PHONY: all clean test test-update test-python demo demo-python plot-demo directories swift-build swift-test swift-clean dist \
        shared asan test-asan coverage-build coverage-report coverage-check \
        fuzz-build fuzz-run test-valgrind bench bench-check bench-gen

all: directories $(TARGET_LIB) $(TARGET_CLI) $(TARGET_COMPARE)

directories:
	@mkdir -p $(BIN_DIR) $(LIB_DIR) $(DIST_DIR) tests/results tests/expected

# Static Library
$(TARGET_LIB): $(OBJECTS)
	ar rcs $@ $^

$(LIB_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# CLI Tool
$(TARGET_CLI): $(SRC_DIR)/main.c $(TARGET_LIB)
	$(CC) $(CFLAGS) $< $(TARGET_LIB) $(TARGET_LIB) -o $@ $(LDFLAGS)

# Test Utility
$(TARGET_COMPARE): $(TEST_DIR)/csv_compare.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

# Tests
MODELS = $(wildcard examples/*.json)
RESULTS = $(patsubst examples/%.json,tests/results/%.csv,$(MODELS))

test: all
	@echo "Running Regression Tests..."
	@mkdir -p tests/results
	@for model in $(MODELS); do \
		name=$$(basename $$model .json); \
		echo -n "Testing $$name... "; \
		./bin/gssk $$model tests/results/$$name.csv > /dev/null 2>&1; \
		if [ -f tests/expected/$$name.csv ]; then \
			./bin/csv_compare tests/expected/$$name.csv tests/results/$$name.csv; \
			if [ $$? -eq 0 ]; then echo "PASSED"; \
			else echo "FAILED"; exit 1; fi; \
		else \
			echo "SKIPPED (No expected output found. Run 'make test-update' to generate)"; \
		fi; \
	done

test-python: shared
	@echo "Running Python binding tests..."
	@python3 python/test_gssk.py -v

demo-python: shared
	@echo "=== Python binding demo ==="
	@python3 python/demo.py

# Quick demo — run two models, print CSV output, and generate PNG plot
demo: all
	@echo "=== Decay model (exponential decay, RK4) ==="
	@$(TARGET_CLI) examples/decay_model.json /tmp/gssk_demo_decay.csv
	@head -6 /tmp/gssk_demo_decay.csv
	@echo ""
	@echo "=== Household model (4-carrier ecological-economy) ==="
	@$(TARGET_CLI) examples/household_model.json /tmp/gssk_demo_household.csv
	@head -3 /tmp/gssk_demo_household.csv
	@echo "... ($$(( $$(wc -l < /tmp/gssk_demo_household.csv) - 1 )) data rows, $$(head -1 /tmp/gssk_demo_household.csv | tr ',' '\n' | wc -l | tr -d ' ') columns)"
	@echo ""
	@python3 python/plot_demo.py

# Standalone plot target — regenerates CSVs then plots
plot-demo: all
	@$(TARGET_CLI) examples/decay_model.json /tmp/gssk_demo_decay.csv > /dev/null 2>&1
	@$(TARGET_CLI) examples/household_model.json /tmp/gssk_demo_household.csv > /dev/null 2>&1
	@python3 python/plot_demo.py

test-update: all
	@echo "Updating Expected Test Outputs..."
	@mkdir -p tests/expected
	@for model in $(MODELS); do \
		name=$$(basename $$model .json); \
		echo "Generating expected output for $$name"; \
		./bin/gssk $$model tests/expected/$$name.csv; \
	done

clean: swift-clean
	rm -rf $(BIN_DIR) $(LIB_DIR) $(DIST_DIR) tests/results coverage/

# ──────────────────────────────────────────────────────────────
# Shared library (required by Python ctypes binding)
# ──────────────────────────────────────────────────────────────
TARGET_SO = $(LIB_DIR)/libgssk.so

shared: directories $(TARGET_SO)

$(TARGET_SO): $(SOURCES)
	$(CC) $(CFLAGS) -shared -o $@ $^ $(LDFLAGS)

# ──────────────────────────────────────────────────────────────
# AddressSanitizer + UndefinedBehaviorSanitizer build
# Requires clang (CC=clang make asan)
# ──────────────────────────────────────────────────────────────
ASAN_FLAGS = -Wall -Wextra -std=c99 -Iinclude -fPIC -g -O1 \
             -fsanitize=address,undefined -fno-omit-frame-pointer

TARGET_ASAN_CLI = $(BIN_DIR)/gssk_asan

asan: directories
	$(CC) $(ASAN_FLAGS) $(SOURCES) $(SRC_DIR)/main.c -o $(TARGET_ASAN_CLI) $(LDFLAGS)

test-asan: asan $(TARGET_COMPARE)
	@echo "Running regression tests under ASan/UBSan..."
	@mkdir -p tests/results
	@for model in $(MODELS); do \
		name=$$(basename $$model .json); \
		echo -n "ASan $$name... "; \
		if [ ! -f tests/expected/$$name.csv ]; then echo "SKIPPED"; continue; fi; \
		ASAN_OPTIONS=detect_leaks=1 \
		./$(TARGET_ASAN_CLI) $$model tests/results/$$name.csv > /dev/null 2>&1; \
		./bin/csv_compare tests/expected/$$name.csv tests/results/$$name.csv; \
		if [ $$? -eq 0 ]; then echo "PASSED"; else echo "FAILED"; exit 1; fi; \
	done

# ──────────────────────────────────────────────────────────────
# gcov / lcov coverage
# ──────────────────────────────────────────────────────────────
COVERAGE_FLAGS = -Wall -Wextra -std=c99 -Iinclude -fPIC -g -O0 \
                 --coverage -fprofile-arcs -ftest-coverage

TARGET_COV_CLI  = $(BIN_DIR)/gssk_cov
TARGET_COV_ADV  = $(BIN_DIR)/test_advanced_cov
COVERAGE_MIN_LINE = 40

coverage-build: directories
	@mkdir -p coverage
	gcc $(COVERAGE_FLAGS) $(SOURCES) $(SRC_DIR)/main.c -o $(TARGET_COV_CLI) -lm
	gcc $(COVERAGE_FLAGS) $(SOURCES) $(TEST_DIR)/test_advanced.c -o $(TARGET_COV_ADV) -lm

coverage-report: coverage-build
	@mkdir -p coverage/html
	@echo "Running tests to collect coverage data..."
	@mkdir -p tests/results
	@for model in $(MODELS); do \
		name=$$(basename $$model .json); \
		./$(TARGET_COV_CLI) $$model tests/results/$$name.csv > /dev/null 2>&1 || true; \
	done
	@echo "Running advanced API tests for coverage..."
	./$(TARGET_COV_ADV) > /dev/null 2>&1 || true
	lcov --capture --directory . --output-file coverage/lcov.info \
	     --ignore-errors mismatch,unused
	lcov --remove coverage/lcov.info '*/cJSON.c' '*/tests/*' \
	     --output-file coverage/lcov.info \
	     --ignore-errors mismatch,unused
	genhtml coverage/lcov.info --output-directory coverage/html --quiet

coverage-check: coverage-report
	@line_pct=$$(lcov --summary coverage/lcov.info 2>&1 | grep 'lines' | grep -oP '[0-9]+\.[0-9]+(?=%)' | head -1); \
	echo "Line coverage: $${line_pct}% (gate: $(COVERAGE_MIN_LINE)%)"; \
	if [ -n "$$line_pct" ] && [ $$(echo "$$line_pct < $(COVERAGE_MIN_LINE)" | bc -l) -eq 1 ]; then \
		echo "FAIL: line coverage below $(COVERAGE_MIN_LINE)%"; exit 1; \
	else echo "OK"; fi

# ──────────────────────────────────────────────────────────────
# Valgrind memory-error check (Linux only)
# ──────────────────────────────────────────────────────────────
VALGRIND = valgrind --error-exitcode=1 --leak-check=full \
           --show-leak-kinds=all --track-origins=yes -q

test-valgrind: all
	@echo "Running regression tests under Valgrind..."
	@mkdir -p tests/results
	@for model in $(MODELS); do \
		name=$$(basename $$model .json); \
		echo -n "Valgrind $$name... "; \
		if [ ! -f tests/expected/$$name.csv ]; then echo "SKIPPED"; continue; fi; \
		$(VALGRIND) ./bin/gssk $$model tests/results/$$name.csv > /dev/null 2>&1; \
		if [ $$? -eq 0 ]; then echo "PASSED"; else echo "FAILED (leaks/errors)"; exit 1; fi; \
	done

# ──────────────────────────────────────────────────────────────
# LibFuzzer target (requires clang with -fsanitize=fuzzer)
# ──────────────────────────────────────────────────────────────
FUZZ_FLAGS  = -Wall -std=c99 -Iinclude -g -O1 \
              -fsanitize=fuzzer,address,undefined
TARGET_FUZZ = $(BIN_DIR)/fuzz_gssk
FUZZ_TIMEOUT ?= 30
FUZZ_CORPUS  = tests/fuzz_corpus

fuzz-build: directories
	clang $(FUZZ_FLAGS) $(SOURCES) $(TEST_DIR)/fuzz_gssk.c -lm -o $(TARGET_FUZZ)

fuzz-run: fuzz-build
	@mkdir -p $(FUZZ_CORPUS)
	./$(TARGET_FUZZ) $(FUZZ_CORPUS) -max_total_time=$(FUZZ_TIMEOUT) \
	    -print_final_stats=1 -jobs=1 2>&1 | tail -20

# ──────────────────────────────────────────────────────────────
# Benchmark suite
# ──────────────────────────────────────────────────────────────
BENCH_BASELINE_MS ?= 500    # wall-clock budget for supply_chain_30 (ms)

bench-gen:
	@echo "Generating benchmark models..."
	@python3 bench/gen_bench_models.py

bench: all bench-gen
	@chmod +x bench/run_bench.sh
	@./bench/run_bench.sh

bench-check: all bench-gen
	@chmod +x bench/run_bench.sh
	@./bench/run_bench.sh --regression $(BENCH_BASELINE_MS)

# Sync dist/ without requiring emscripten (schema + TypeScript declarations)
dist: directories
	cp $(SRC_DIR)/gssk.d.ts $(DIST_DIR)/gssk.d.ts
	cp gssk.schema.json $(DIST_DIR)/gssk.schema.json

# WASM Build (Requires emscripten)
WASM_EXPORTS = ["_GSSK_Init","_GSSK_Step","_GSSK_Reset","_GSSK_GetState","_GSSK_GetStateSize",\
"_GSSK_GetTStart","_GSSK_GetTEnd","_GSSK_GetDt","_GSSK_GetCurrentTime","_GSSK_GetStepCount",\
"_GSSK_GetNodeID","_GSSK_FindNodeIdx","_GSSK_GetEdgeID","_GSSK_FindEdgeIdx",\
"_GSSK_GetEdgeCount","_GSSK_GetEdgeK","_GSSK_SetEdgeK",\
"_GSSK_GetTransformationRatio","_GSSK_GetQualityFlow","_GSSK_GetEdgeQualityFlow",\
"_GSSK_GetSolverConfidence","_GSSK_AddNode","_GSSK_AddEdge","_GSSK_DeactivateEdge",\
"_GSSK_DeactivateNode","_GSSK_ReclassifyNetwork",\
"_GSSK_SerializeModel","_GSSK_SerializeSnapshot","_GSSK_FreeString",\
"_GSSK_GetSchemaVersion","_GSSK_GetModelName","_GSSK_GetModelDescription",\
"_GSSK_GetModelKernelVersion","_GSSK_GetModelHash","_GSSK_GetVersionString","_GSSK_GetVersionCode",\
"_GSSK_GetEdgeErrorEstimate","_GSSK_GetStepErrorEstimate",\
"_GSSK_GetEventCount","_GSSK_GetEventTime","_GSSK_GetEventEdgeID","_GSSK_GetEventDirection",\
"_GSSK_EnsembleForecast","_GSSK_FreeEnsembleResult","_GSSK_Calibrate",\
"_GSSK_GetErrorDescription","_GSSK_Free","_malloc","_free",\
"_GSSK_StepAdaptive","_GSSK_GetLastStepSize","_GSSK_GetNextStepSize",\
"_GSSK_GetConservationError","_GSSK_SetDiagHooks",\
"_GSSK_EnableForwardSensitivity","_GSSK_DisableForwardSensitivity","_GSSK_GetSensitivity",\
"_GSSK_RunAdjoint","_GSSK_GetTransformitySensitivity",\
"_GSSK_CalibrateGradient","_GSSK_CalibrateMonteCarlo",\
"_GSSK_GetMutationCount","_GSSK_GetMutationRecord","_GSSK_SetMutationCause",\
"_GSSK_ClearMutationLog","_GSSK_ExportMutationLog","_GSSK_Replay",\
"_GSSK_GetCarrierCount","_GSSK_GetCarrier","_GSSK_GetNodeCarrier",\
"_GSSK_GetEdgeCarrier","_GSSK_GetCarrierConservationError"]

wasm: dist
	emcc $(SRC_DIR)/gssk.c $(SRC_DIR)/advanced.c $(SRC_DIR)/cJSON.c -Iinclude -O3 -s WASM=1 \
	-s MODULARIZE=1 -s EXPORT_NAME='createGSSK' \
	-s EXPORTED_FUNCTIONS='$(WASM_EXPORTS)' \
	-s EXPORTED_RUNTIME_METHODS='["ccall","cwrap","stringToUTF8","UTF8ToString","lengthBytesUTF8","allocate","ALLOC_NORMAL","HEAPU8","HEAPF64","HEAPU32"]' \
	-o $(DIST_DIR)/gssk.js

# ──────────────────────────────────────────────────────────────
# Swift Package (Requires Swift toolchain)
# ──────────────────────────────────────────────────────────────

# Build the Swift package (CGSSK + GSSK wrapper)
swift-build:
	@command -v swift >/dev/null 2>&1 || { echo "swift not found — install Xcode or swift.org toolchain"; exit 1; }
	swift build

# Run the Swift test suite
swift-test:
	@command -v swift >/dev/null 2>&1 || { echo "swift not found — install Xcode or swift.org toolchain"; exit 1; }
	swift test

# Remove Swift build artefacts (.build/ directory)
swift-clean:
	@command -v swift >/dev/null 2>&1 && swift package clean || rm -rf .build
