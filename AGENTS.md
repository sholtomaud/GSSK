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
3. `make test-advanced` and `make test-price-node` pass.
4. `make wasm-container` builds `dist/gssk.js` and `dist/gssk.wasm`. This runs
   Emscripten in a Linux container, so it works without a local `emcc` — there
   is no longer an "if available" excuse for shipping unverified WASM.
5. `make test-linux` builds and tests under **real GCC**. `/usr/bin/gcc` on macOS
   is a symlink to Apple clang, so a clean local build is NOT evidence of a clean
   CI build; GCC emits diagnostics clang does not and `CFLAGS` carries `-Werror`.

`make ci-local` runs 4 and 5 together.

`make demo` is also containerised, for a different reason: it plots with
matplotlib, which a bare system `python3` does not have. `Containerfile.demo`
carries a uv-managed interpreter and a pinned matplotlib
(`python/requirements-demo.txt`) alongside the C toolchain, so `make demo`
needs nothing installed on the host but the `container` CLI. It cleans first
and leaves Linux artefacts in `lib/`, so run `make clean && make all`
afterwards. `make demo-native` is the same demo without the container, for a
host that does have matplotlib.

## 🔀 Contribution Workflow

**Never commit directly to `main`.** Branch, push, and open a PR.

1. **Branch** — `feat/<task-slug>`, `fix/<slug>`, or `docs/<slug>`.
2. **Commit** — explain *why*, not just what. If you found a defect while doing
   something else, say so in the message; that is often the most valuable part.
3. **Push AND open a PR.** `gh pr create`. A pushed branch with no PR is not
   finished work: CI runs on `pull_request`, so a bare push produces **no test
   signal at all** and there is nothing for a reviewer to merge on.
4. **Wait for CI to pass.** All checks green — including `WASM build
   (emscripten)`, which exists specifically because export-list breakage used to
   reach `main` unverified.
5. **Do not merge your own PR.** Merging is the maintainer's decision. Hand over
   a green PR, not a branch.

### Reporting
State what you verified and what you did not. If a check was skipped or a test
failed, say so plainly with the output. "Should work" is not a result. Where you
made a judgement call under ambiguity, name the assumption in the PR body.

### Finishing a task
Work is not done when the code is written. It is done when it is **merged**.
A task marked complete whose code sits on an unmerged branch is a silent
blocker for everything downstream of it — this has already happened once here
(`c0-price-constant-or-node-ref`, whose stranded branch blocked Phase C until
it was found). Before marking a task done, confirm its code is on `main`.

## 📂 Directory Structure
- `include/`: Public headers.
- `src/`: Implementation.
- `bin/`: Built binaries (CLI, Compare tool).
- `lib/`: Compiled libraries.
- `tests/expected/`: "Gold Standard" results for regression.
- `examples/`: Reference JSON models.

## UPDATES PRE-COMMIT CHECK

- update the TODO.md to check off any completed items ythat are complete and tested.