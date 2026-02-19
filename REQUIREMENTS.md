# General Systems Simulation Kernel (GSSK)

## 1. Overview
The GSSK is a high-performance numerical engine designed to simulate complex systems based on General Systems Theory and Energy Systems Language (Odum). It consumes a model topology in JSON format and generates time-series state trajectories.

## 2. Core Functional Requirements

### 2.1 Model Parsing & Initialization
- **Schema Validation**: Must validate the incoming JSON model against a strict schema (e.g., verifying that every "flow" has a valid "source" and "sink").
- **Dynamic Allocation**: The kernel must dynamically allocate memory for components (storages, flows, sources) based on the JSON definition to support models ranging from 1 to 10,000+ nodes.
- **Initial State Setup**: Initialize all state variables ($Q$) and global constants (e.g., simulation duration, $\Delta t$).

### 2.2 Numerical Engine (The Solver)
- **Integration Method**: Support Euler integration as a baseline, with modular support for Runge-Kutta (RK4) for high-precision needs.
- **Isomorphic Logic**: Implement standard GST mathematical primitives:
  - **Linear Outflow**: $k \cdot Q$
  - **Work Gates (Interaction)**: $k \cdot Q_1 \cdot Q_2$
  - **Limiting Factors**: Michaelis-Menten or Liebig’s Law of the Minimum logic.
  - **Switching Logic**: Threshold-based logic (e.g., if $Q > x$ then flow starts).
- **Time-Step Management**: Support fixed $\Delta t$ and provide hooks for adaptive time-stepping to prevent numerical "blow-up" in stiff systems.

## 3. Input Specification (JSON Schema)
The kernel must recognize three primary object types:

- **Nodes (Storages/Sources)**:
  - `id`: Unique identifier.
  - `type`: "source", "storage", "sink", or "constant".
  - `value`: Initial quantity or constant rate.
- **Edges (Flows)**:
  - `id`: Unique identifier.
  - `origin`: Source node ID.
  - `target`: Destination node ID.
  - `logic`: The mathematical relationship (e.g., "force_dependent", "interaction").
  - `k`: Conductivity or coefficients.
- **Global Config**:
  - `t_start`, `t_end`, `dt`.

## 4. Output Modes & Interface

### 4.1 Bulk Output (Batch)
- **Typed Array Buffer**: Provide a raw memory pointer to a flat float array (e.g., `[t0, q1, q2, t1, q1, q2...]`) for high-speed transfer to WASM/JS.
- **CSV/JSON**: Support serialization to standard file formats for CLI usage.

### 4.2 Streaming Output (Real-time)
- **Callback Hook**: The kernel must trigger a "tick" event after every calculation step.
- **JSONL Emission**: Output one line of JSON per timestamp for pipe-based CLI workflows or real-time web charts.

## 5. Non-Functional Requirements
- **Portability**: Written in ISO C99 for compatibility with gcc, clang, and emcc (Emscripten).
- **Memory Safety**: Zero use of global state variables to allow for "Thread Safety," enabling multiple kernels to run in parallel (WebWorkers/Threads).
- **Speed**: Capable of simulating 1,000 iterations for a 100-node model in under 10ms on modern hardware.
- **No External Dependencies**: The core kernel must not rely on external C libraries (except standard math/string headers) to ensure a lightweight WASM binary.

## 6. Execution Environments
- **CLI**: Accepts file paths for JSON input and redirected output (`gssk -i model.json > output.jsonl`).
- **WASM**: Exports `run_step()` and `get_buffer()` functions to be called by the JavaScript Event Loop.
- **Embedded/API**: Clean headers for integration into larger C/C++ host applications.

## Philosophical Note on Naming
In alignment with the user's preference, all terminology within the code and documentation regarding "systems" or "feedback" should be treated as technical definitions. Double quotes in this document are reserved strictly for quoting an author or identifying literal string keys in the JSON schema.