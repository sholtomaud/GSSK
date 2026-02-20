# GSSK Technical Specification

## 1. Mathematical Foundation

The GSSK simulates systems as a set of coupled Ordinary Differential Equations (ODEs). The state of the system is represented by a vector $Q$, where each $Q_i$ corresponds to the quantity in a storage node.

### 1.1 Fundamental Equation
For each storage node $i$, the rate of change is:
$$\frac{dQ_i}{dt} = \sum F_{in} - \sum F_{out}$$

### 1.2 Flow Logic Primitives
Flow rates ($F$) are calculated based on the `logic` attribute of an edge.

| Logic Type | Equation | Description |
| :--- | :--- | :--- |
| `constant` | $F = k$ | Source-driven constant flow. |
| `linear` | $F = k \cdot Q_{origin}$ | Flow proportional to source (linear decay). |
| `interaction` | $F = k \cdot Q_{origin} \cdot Q_{control}$ | Work gate (multiplier logic). |
| `limit` | $F = \frac{k \cdot Q_{origin}}{1 + (Q_{origin}/C)}$ | Michaelis-Menten / Saturation logic. |
| `threshold` | $F = (Q_{origin} > \theta) ? k : 0$ | Boolean switching logic. |

---

## 2. JSON Schema Specification (v1.0)

Models MUST be provided as a single JSON object.

### 2.1 The `nodes` Array
Each node represents a state variable or a boundary condition.
```json
{
  "id": "string (unique)",
  "type": "storage | source | sink | constant",
  "value": "float64 (initial state or constant value)"
}
```

### 2.2 The `edges` Array
Each edge represents a flow of energy/matter between nodes.
```json
{
  "id": "string (unique)",
  "origin": "string (source node id)",
  "target": "string (destination node id)",
  "logic": "string (constant | linear | interaction | limit | threshold)",
  "params": {
    "k": "float64 (conductivity)",
    "control_node": "string (optional, for interaction/limit logic)",
    "threshold": "float64 (optional, for threshold logic)"
  }
}
```

### 2.3 The `config` Object
```json
{
  "t_start": 0.0,
  "t_end": 100.0,
  "dt": 0.1,
  "method": "euler | rk4"
}
```

---

## 3. C API & Lifecycle (ABI)

The kernel is implemented in C99, exposing the following interface for host environments (WASM/CLI).

### 3.1 Data Structures
```c
typedef struct {
    double* state;      // Current Q vector
    double* dQ;         // Rate of change vector
    int node_count;
    // ... internal topology pointers ...
} GSSK_Instance;
```

### 3.2 Functions
| Function | Signature | Description |
| :--- | :--- | :--- |
| `GSSK_Init` | `GSSK_Instance* GSSK_Init(const char* json_data)` | Parses JSON and allocates all internal memory. Returns NULL on schema failure. |
| `GSSK_Step` | `void GSSK_Step(GSSK_Instance* inst, double dt)` | Performs one integration step (Euler/RK4). |
| `GSSK_GetState` | `const double* GSSK_GetState(GSSK_Instance* inst)` | Returns pointer to the internal state buffer for reading. |
| `GSSK_Free` | `void GSSK_Free(GSSK_Instance* inst)` | Safely deallocates all instance memory. |

---

## 4. Error Handling Policy

The kernel uses a **Fail-Safe** numerical policy:
1. **Schema Errors**: `GSSK_Init` returns a structured `GSSK_Error` object detailing the line/node causing the failure.
2. **Numerical Divergence**: If any $Q_i$ becomes `NaN` or `Inf`, the simulation pauses and sets an internal `ERR_DIVERGENCE` flag.
3. **Negative Flows**: By default, physical conservation is enforced. $Q_i$ cannot drop below $0.0$. If a flow would cause a negative state, it is clamped to $0.0$.

---

## 5. Memory & Concurrency

- **No Statics**: The kernel contains zero global or static variables.
- **Thread Locality**: A `GSSK_Instance` is entirely self-contained. Multiple instances can be run on separate threads without locking.
- **Alignment**: State vectors are 32-byte aligned to support SIMD (AVX/SSE) optimizations in the solver loop.

---

## 6. Verification & Validation (Benchmarks)

To ensure numerical correctness and performance stability, the kernel must pass the following validation suite.

### 6.1 Reference Models
The `tests/models/` directory contains JSON files and corresponding expected CSV trajectories.

| Model ID | Logic Tested | Success Criteria |
| :--- | :--- | :--- |
| `01_decay` | Linear Outflow | $Q(t) = Q_0 e^{-kt}$, Error $< 10^{-7}$ (RK4) |
| `02_multiplier` | Interaction | Convergence to steady state within $1\%$ |
| `03_saturation` | Limit Logic | Precise match to Michaelis-Menten curve |
| `04_oscillator` | Feedback Loop | Conservation of energy within $0.01\%$ over 1k steps |

### 6.2 Performance Targets
Benchmarks must be run on a single thread.
- **Latency**: 10,000 steps for a 100-node model in $< 50ms$.
- **Throughput**: $> 1,000,000$ flow calculations per second on modern x86/ARM hardware.

### 6.3 Build & CI Standards
- **Compiler**: `gcc` or `clang` with `-Wall -Wextra -Werror -std=c99`.
- **Sanitizers**: Must run clean under `valgrind` (zero leaks) and AddressSanitizer (ASan).
- **WASM**: Must compile via `emcc` without modifications.
