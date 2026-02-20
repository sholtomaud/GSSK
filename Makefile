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
SOURCES = $(SRC_DIR)/gssk.c $(SRC_DIR)/cJSON.c
OBJECTS = $(LIB_DIR)/gssk.o $(LIB_DIR)/cJSON.o
TARGET_LIB = $(LIB_DIR)/libgssk.a
TARGET_CLI = $(BIN_DIR)/gssk
TARGET_COMPARE = $(BIN_DIR)/csv_compare

.PHONY: all clean test test-update directories

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

test-update: all
	@echo "Updating Expected Test Outputs..."
	@mkdir -p tests/expected
	@for model in $(MODELS); do \
		name=$$(basename $$model .json); \
		echo "Generating expected output for $$name"; \
		./bin/gssk $$model tests/expected/$$name.csv; \
	done

clean:
	rm -rf $(BIN_DIR) $(LIB_DIR) $(DIST_DIR) tests/results

# WASM Build (Requires emscripten)
wasm: directories
	cp $(SRC_DIR)/gssk.d.ts $(DIST_DIR)/gssk.d.ts
	emcc $(SRC_DIR)/gssk.c $(SRC_DIR)/cJSON.c -Iinclude -O3 -s WASM=1 \
	-s MODULARIZE=1 -s EXPORT_NAME='createGSSK' \
	-s EXPORTED_FUNCTIONS='["_GSSK_Init", "_GSSK_Step", "_GSSK_GetState", "_GSSK_GetStateSize", "_GSSK_GetTStart", "_GSSK_GetTEnd", "_GSSK_GetDt", "_GSSK_GetNodeID", "_GSSK_Free", "_malloc", "_free"]' \
	-s EXPORTED_RUNTIME_METHODS='["ccall", "cwrap", "stringToUTF8", "UTF8ToString", "lengthBytesUTF8", "allocate", "ALLOC_NORMAL", "HEAPU8", "HEAPF64"]' \
	-o $(DIST_DIR)/gssk.js
