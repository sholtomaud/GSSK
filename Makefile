# GSSK Build System
CC      ?= gcc
CFLAGS  = -Wall -Wextra -Werror -std=c99 -Iinclude -fPIC
LDFLAGS = -lm

# Architecture flags.
#
# -march=native was previously unconditional. It bakes the build machine's CPU
# into the binary, which is wrong for anything distributed, and it is actively
# fragile: clang rejects some auto-detected feature combinations outright, so a
# CI job landing on an AVX10.1-capable runner fails with
#   error: invalid feature combination: +avx10.1-256 ... [-Winvalid-feature-combination]
# under -Werror, while the identical source builds fine on an older runner.
# That made green-ness depend on which machine picked the job up.
#
# Default is now portable. Opt in with NATIVE=1 for local benchmarking, where
# tuning to the host is the point and reproducibility across machines is not.
ifeq ($(NATIVE), 1)
	ARCH_FLAGS = -march=native
else
	ARCH_FLAGS =
endif

# Optimization levels (Use 'make DEBUG=1' for debugging)
ifeq ($(DEBUG), 1)
	CFLAGS += -g -O0 -DDEBUG
else
	CFLAGS += -O3 $(ARCH_FLAGS)
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

# ──────────────────────────────────────────────────────────────
# Containerised Linux toolchains (Apple `container` CLI)
#
# Two things macOS cannot verify locally:
#   emcc — no emsdk, and CI only builds WASM in the deploy job, which does
#          not run on pull requests.
#   gcc  — /usr/bin/gcc here is Apple clang; real GCC emits warnings clang
#          does not, and CFLAGS carries -Werror.
#
# Both emsdk and ubuntu images are amd64-only or resolve wrong on Apple
# silicon, so --platform is explicit: without it `container run` fails with
# "platform linux/arm64" even when the image built fine.
# ──────────────────────────────────────────────────────────────
CONTAINER_BIN    := container
CONTAINER_PLATFORM := linux/amd64
IMAGE_WASM       := gssk-wasm
IMAGE_LINUX      := gssk-linux
EMSDK_VERSION    := 3.1.64
UBUNTU_VERSION   := 24.04
CWORKDIR         := /work
CRUN              = $(CONTAINER_BIN) run --rm --platform $(CONTAINER_PLATFORM) -v $(shell pwd):$(CWORKDIR)

.PHONY: all clean test test-update test-advanced test-price-node test-ratio test-delivered-work test-price-dynamics test-node-types test-carrier-api test-schema test-python demo demo-python plot-demo directories swift-build swift-test swift-clean dist \
        shared asan test-asan coverage-build coverage-report coverage-check \
        fuzz-build fuzz-run test-valgrind bench bench-check bench-gen \
        container-start container-image container-image-wasm container-image-linux \
        wasm-container test-linux test-linux-clang shell-wasm shell-linux ci-local

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

test: all test-schema
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

# Advanced API test suite (calibration, ensemble, Phase 7 node types)
TARGET_TEST_ADV = $(BIN_DIR)/test_advanced

$(TARGET_TEST_ADV): $(TEST_DIR)/test_advanced.c $(TARGET_LIB)
	$(CC) $(CFLAGS) $< $(TARGET_LIB) -o $@ $(LDFLAGS)

test-advanced: all $(TARGET_TEST_ADV)
	@echo "Running advanced API tests..."
	@./$(TARGET_TEST_ADV)

# Phase C.0 — price_node reference resolution, round-trip, constant fallback
TARGET_TEST_PRICE = $(BIN_DIR)/test_price_node

$(TARGET_TEST_PRICE): $(TEST_DIR)/test_price_node.c $(TARGET_LIB)
	$(CC) $(CFLAGS) $< $(TARGET_LIB) -o $@ $(LDFLAGS)

test-price-node: all $(TARGET_TEST_PRICE)
	@echo "Running price_node tests..."
	@./$(TARGET_TEST_PRICE)

# Phase C.1 — ratio (division) logic: hand-calculated quotient, epsilon floor,
# RK4 vs IDC agreement.
TARGET_TEST_RATIO = $(BIN_DIR)/test_ratio

$(TARGET_TEST_RATIO): $(TEST_DIR)/test_ratio.c $(TARGET_LIB)
	$(CC) $(CFLAGS) $< $(TARGET_LIB) -o $@ $(LDFLAGS)

test-ratio: all $(TARGET_TEST_RATIO)
	@echo "Running ratio logic tests..."
	@./$(TARGET_TEST_RATIO)

# Phase C.2 — delivered work signal: tracking, and non-perturbation of the
# trade it observes.
TARGET_TEST_DW = $(BIN_DIR)/test_delivered_work

$(TARGET_TEST_DW): $(TEST_DIR)/test_delivered_work.c $(TARGET_LIB)
	$(CC) $(CFLAGS) $< $(TARGET_LIB) -o $@ $(LDFLAGS)

test-delivered-work: all $(TARGET_TEST_DW)
	@echo "Running delivered-work tests..."
	@./$(TARGET_TEST_DW)

# Phase C.3 — price dynamics: relaxation toward M/W, the named ratio numerator,
# and the solver paths that only disagree when the Jacobian column is wrong.
TARGET_TEST_PRICEDYN = $(BIN_DIR)/test_price_dynamics

$(TARGET_TEST_PRICEDYN): $(TEST_DIR)/test_price_dynamics.c $(TARGET_LIB)
	$(CC) $(CFLAGS) $< $(TARGET_LIB) -o $@ $(LDFLAGS)

test-price-dynamics: all $(TARGET_TEST_PRICEDYN)
	@echo "Running price dynamics tests..."
	@./$(TARGET_TEST_PRICEDYN)

# Node type validation — an unrecognised node `type` must be an error, not a
# silent fallback to `storage` (ADR 0004). Covers both call sites: GSSK_Init,
# which has full archetype dispatch, and GSSK_AddNode, which has none.
TARGET_TEST_NODETYPE = $(BIN_DIR)/test_node_type_validation

$(TARGET_TEST_NODETYPE): $(TEST_DIR)/test_node_type_validation.c $(TARGET_LIB)
	$(CC) $(CFLAGS) $< $(TARGET_LIB) -o $@ $(LDFLAGS)

test-node-types: all $(TARGET_TEST_NODETYPE)
	@echo "Running node type validation tests..."
	@./$(TARGET_TEST_NODETYPE)

# Carrier accessors — the flat getters that keep GSSK_Carrier's struct layout
# from crossing the WASM boundary. Also asserts the flat path and
# GSSK_GetCarrier cannot drift apart.
TARGET_TEST_CARRIER = $(BIN_DIR)/test_carrier_api

$(TARGET_TEST_CARRIER): $(TEST_DIR)/test_carrier_api.c $(TARGET_LIB)
	$(CC) $(CFLAGS) $< $(TARGET_LIB) -o $@ $(LDFLAGS)

test-carrier-api: all $(TARGET_TEST_CARRIER)
	@echo "Running carrier accessor tests..."
	@./$(TARGET_TEST_CARRIER)

# Schema conformance — examples/ must match gssk.schema.json.
#
# The kernel does not validate against the schema at load time (ADR 0004), so
# this is what stops the schema and the parser drifting apart. It skips when
# jsonschema is absent rather than failing, so a bare checkout still builds;
# CI installs the dependency so the gate is real there.
# Serialised output is checked too: the schema also has to describe what
# GSSK_SerializeModel/Snapshot emit, which is the format the archival story
# in Phase G rests on. dump_serialized writes those into tests/results.
SER_DIR = tests/results/serialized
TARGET_DUMP_SER = $(BIN_DIR)/dump_serialized

$(TARGET_DUMP_SER): $(TEST_DIR)/dump_serialized.c $(TARGET_LIB)
	$(CC) $(CFLAGS) $< $(TARGET_LIB) -o $@ $(LDFLAGS)

test-schema: directories $(TARGET_DUMP_SER)
	@rm -rf $(SER_DIR) && mkdir -p $(SER_DIR)
	@./$(TARGET_DUMP_SER) $(SER_DIR) $(MODELS) $(wildcard tests/schema_fixtures/*.json)
	@python3 scripts/validate_models.py

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
COVERAGE_MIN_LINE = 35

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
"_GSSK_GetCarrierID","_GSSK_GetCarrierUnit","_GSSK_GetCarrierConserved",\
"_GSSK_FindCarrierIdx",\
"_GSSK_GetEdgeCarrier","_GSSK_GetCarrierConservationError",\
"_GSSK_GetNodeTypeString",\
"_GSSK_GetArchetypeCount","_GSSK_GetArchetypeName",\
"_GSSK_GetCompositeCount","_GSSK_GetCompositeID",\
"_GSSK_GetCompositeArchetype","_GSSK_GetNodeComposite","_GSSK_GetNodeRole",\
"_GSSK_GetCompositeMemberCount","_GSSK_GetCompositeMemberIndex",\
"_GSSK_SetSeed","_GSSK_GetSeed","_GSSK_NextRandom","_GSSK_NextRandomUniform"]

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

# ──────────────────────────────────────────────────────────────
# Containerised Linux builds
#
# `make wasm` and a real-GCC build cannot run on macOS directly. These
# targets run them in Linux containers via the Apple `container` CLI.
# Artefacts land in the bind-mounted working tree exactly as a native
# build would.
# ──────────────────────────────────────────────────────────────

# Start the container system daemon (idempotent)
container-start:
	@$(CONTAINER_BIN) system start >/dev/null 2>&1 || true

# Build the Emscripten image (matches deploy.yml's emsdk pin)
container-image-wasm: container-start
	$(CONTAINER_BIN) build -f Containerfile -t $(IMAGE_WASM) \
		--platform $(CONTAINER_PLATFORM) \
		--build-arg EMSDK_VERSION=$(EMSDK_VERSION) .

# Build the native Linux image (matches CI's ubuntu-latest)
container-image-linux: container-start
	$(CONTAINER_BIN) build -f Containerfile.linux -t $(IMAGE_LINUX) \
		--platform $(CONTAINER_PLATFORM) \
		--build-arg UBUNTU_VERSION=$(UBUNTU_VERSION) .

# Build both images
container-image: container-image-wasm container-image-linux

# Build the WASM artefacts into dist/. This is the check that CI does NOT
# run on pull requests, so run it before pushing anything that touches
# WASM_EXPORTS or any exported symbol.
wasm-container: container-image-wasm
	$(CRUN) $(IMAGE_WASM) make wasm

# Full native build + both test suites under real GCC with -Werror.
test-linux: container-image-linux
	$(CRUN) $(IMAGE_LINUX) sh -c 'make clean && make CC=gcc all && make CC=gcc test && make CC=gcc test-advanced && make CC=gcc test-node-types && make CC=gcc test-carrier-api && make CC=gcc test-price-node test-ratio test-delivered-work test-price-dynamics'

# Same under Linux clang, the other half of CI's build-native matrix.
test-linux-clang: container-image-linux
	$(CRUN) $(IMAGE_LINUX) sh -c 'make clean && make CC=clang all && make CC=clang test && make CC=clang test-advanced'

# Everything CI would catch that macOS cannot: both Linux compilers plus WASM.
# Leaves the tree holding Linux objects — run `make clean && make all` after.
ci-local: test-linux test-linux-clang wasm-container
	@echo "──────────────────────────────────────────────"
	@echo "Linux gcc + clang and WASM all built."
	@echo "Tree now holds Linux artefacts; run 'make clean && make all' to restore native."

# Interactive shells for debugging a container build
shell-wasm: container-image-wasm
	$(CONTAINER_BIN) run --rm -it --platform $(CONTAINER_PLATFORM) -v $(shell pwd):$(CWORKDIR) $(IMAGE_WASM) bash

shell-linux: container-image-linux
	$(CONTAINER_BIN) run --rm -it --platform $(CONTAINER_PLATFORM) -v $(shell pwd):$(CWORKDIR) $(IMAGE_LINUX) bash

# ──────────────────────────────────────────────────────────────
# Documents (LaTeX)
#
# Sources live in doco/. latexmk is pointed at doco/build/ via -outdir so
# every transient file (.aux, .bbl, .fls, …) stays out of the source tree;
# doco/.gitignore covers that directory. Requires a TeX distribution —
# these targets are not part of `make all` and never gate a code change.
# ──────────────────────────────────────────────────────────────
DOCO_DIR   = doco
DOCO_BUILD = $(DOCO_DIR)/build
# -cd does not compose with a relative -outdir here (output lands beside the
# source), so the recipes cd explicitly and keep -outdir relative to that.
LATEXMK    = latexmk -pdf -interaction=nonstopmode -halt-on-error -outdir=build

# Report wherever the PDF actually landed. A stock latexmk honours -outdir and
# writes to doco/build/; a wrapper that drops caller flags (this machine has a
# container-backed latexmk shim that passes only the filename through) writes
# beside the source instead. Both locations are gitignored.
report_pdf = ls -1 $(DOCO_BUILD)/$(1).pdf $(DOCO_DIR)/$(1).pdf 2>/dev/null | head -1 | sed 's/^/→ /'

.PHONY: doco whitepaper article doco-clean

# Build both documents
doco: whitepaper article

whitepaper:
	@command -v latexmk >/dev/null 2>&1 || { echo "latexmk not found — install a TeX distribution (e.g. MacTeX, TeX Live)"; exit 1; }
	cd $(DOCO_DIR) && $(LATEXMK) whitepaper.tex
	@$(call report_pdf,whitepaper)

article:
	@command -v latexmk >/dev/null 2>&1 || { echo "latexmk not found — install a TeX distribution (e.g. MacTeX, TeX Live)"; exit 1; }
	cd $(DOCO_DIR) && $(LATEXMK) article.tex
	@$(call report_pdf,article)

# Remove LaTeX build output only; leaves sources untouched.
doco-clean:
	rm -rf $(DOCO_BUILD)
	rm -f $(DOCO_DIR)/*.aux $(DOCO_DIR)/*.bbl $(DOCO_DIR)/*.blg \
	      $(DOCO_DIR)/*.fdb_latexmk $(DOCO_DIR)/*.fls $(DOCO_DIR)/*.log \
	      $(DOCO_DIR)/*.out $(DOCO_DIR)/*.toc $(DOCO_DIR)/*.pdf
