# AGENTS.md: AI Developer Guidelines for GSSK

Welcome, fellow agent. This document provides the necessary context and standards for contributing to the General Systems Simulation Kernel (GSSK).

## 🎯 Project Overview
GSSK is a high-performance numerical engine for General Systems Theory and Odum Energy Systems Language. It simulates systems as coupled Ordinary Differential Equations (ODEs) using Euler or RK4 integration.

## 🛠 Tech Stack
- **Language**: C99 (Strict compliance: `-std=c99 -Wall -Wextra -Werror`).
- **JSON Parser**: `cJSON` (embedded in `src/cJSON.c`).
- **Build System**: GNU Make.
- **IDE Support**: `.clangd`, `compile_flags.txt`, and `compile_commands.json` are maintained in the root.

## 🏗 Architecture
- **Public API**: `include/gssk.h`. DO NOT put implementation details here.
- **Core Logic**: `src/gssk.c`.
- **Instance-Based**: All state is stored in an opaque `GSSK_Instance`. No global or static variables.
- **Memory Management**: Every `GSSK_Init` must have a corresponding `GSSK_Free`.

## 🧪 Testing & Verification
We use a **Registration-based Regression Testing** system.
- **Run Tests**: `make test`
- **Add New Test**: Create a JSON model in `examples/`, then run `make test-update`.
- **Comparison**: We use `bin/csv_compare` with a $10^{-6}$ tolerance to verify numerical stability.

## 📜 Coding Standards
1. **Absolute Paths**: When using IDE tools, prefer absolute paths for configuration (see `.clangd`).
2. **Fail-Safe Policy**:
   - Always check for `NaN` or `Inf` after a step.
   - Enforce physical conservation (clamping $Q < 0$ to $0.0$).
   - JSON parsing must be strict. Return `NULL` if the schema is violated.
3. **No Placeholders**: Never use placeholder code. If a feature isn't implemented, return a proper error code or use `(void)` for intentional stubs.

## 🚀 Deployment (WASM)
GSSK is designed for web integration. Ensure any core changes are compatible with `emcc` (Emscripten).
Command: `make wasm`

## ✅ Programmatic Checks
Before submitting any changes, you MUST ensure:
1. `make clean && make` completes without any errors or warnings.
2. `make test` passes all regression tests with "PASSED" status.
3. `make wasm` (if `emcc` is available) generates a valid `dist/gssk.js` and `dist/gssk.wasm`.

## 📂 Directory Structure
- `include/`: Public headers.
- `src/`: Implementation.
- `bin/`: Built binaries (CLI, Compare tool).
- `lib/`: Compiled libraries.
- `tests/expected/`: "Gold Standard" results for regression.
- `examples/`: Reference JSON models.
