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

## 2. JSON Schema Specification (v2.0)

Models MUST be provided as a single JSON object. v2.0 introduces support for Odum's four-position inter-block array.

### 2.1 The `nodes` Array
Each node represents a state variable or a boundary condition.
```json
{
  "id": "string (unique, max 63 chars)",
  "type": "storage | source | sink | constant",
  "value": "float64 (initial state or constant value)",
  "quality_input": "float64 (optional, Odum Position 4 - boundary Transformity)",
  "output_mode": "partition | replicate (optional, default: partition)"
}
```

### 2.2 The `edges` Array
Each edge represents a flow between nodes, implementing the full Odum connector.
```json
{
  "id": "string (optional, stable identifier)",
  "origin": "string (source node id)",
  "target": "string (destination node id)",
  "carrier": "string (optional, Odum Position 1 - substance type)",
  "logic": "string (constant | linear | interaction | limit | threshold)",
  "params": {
    "k": "float64 (conductivity)",
    "control_node": "string (optional, for interaction/limit logic)",
    "threshold": "float64 (optional, for threshold logic)"
  },
  "output_mode": "partition | replicate (optional, overrides node mode)",
  "coupled_edge": "string (optional, ID of paired counter-flow edge)"
}
```

### 2.3 The `config` Object
```json
{
  "t_start": 0.0,
  "t_end": 100.0,
  "dt": 0.1,
  "method": "auto | euler | rk4 | incipient",
  "solver_tolerance": 1e-6
}
```

---

## 3. C API & Lifecycle (ABI)

The kernel is implemented in C99, exposing the following interface.

### 3.1 Lifecycle
| Function | Signature | Description |
| :--- | :--- | :--- |
| `GSSK_Init` | `GSSK_Status GSSK_Init(const char* json, GSSK_Instance** out)` | Initialises instance from JSON. |
| `GSSK_Step` | `GSSK_Status GSSK_Step(GSSK_Instance* inst, double dt)` | Performs one step with dual-solver cross-validation in `auto` mode. |
| `GSSK_Reset` | `void GSSK_Reset(GSSK_Instance* inst)` | Resets state to initial conditions. |
| `GSSK_Free` | `void GSSK_Free(GSSK_Instance* inst)` | Deallocates all memory. |

### 3.2 State & Quality Accessors
| Function | Signature | Description |
| :--- | :--- | :--- |
| `GSSK_GetState` | `const double* GSSK_GetState(GSSK_Instance* inst)` | Current state vector $Q$. |
| `GSSK_GetTransformationRatio` | `const double* GSSK_GetTransformationRatio(GSSK_Instance* inst)` | Current node transformities ($Tr$). |
| `GSSK_GetQualityFlow` | `const double* GSSK_GetQualityFlow(GSSK_Instance* inst)` | Node inflow emPower ($Tr \cdot F$). |
| `GSSK_GetSolverConfidence` | `GSSK_SolverConfidence GSSK_GetSolverConfidence(GSSK_Instance* inst)` | Returns HIGH or DEGRADED. |

### 3.3 Topology Mutation
| Function | Signature | Description |
| :--- | :--- | :--- |
| `GSSK_AddNode` | `GSSK_Status GSSK_AddNode(GSSK_Instance* inst, const char* json)` | Adds node at runtime. |
| `GSSK_AddEdge` | `GSSK_Status GSSK_AddEdge(GSSK_Instance* inst, const char* json)` | Adds edge at runtime. |
| `GSSK_DeactivateNode` | `GSSK_Status GSSK_DeactivateNode(GSSK_Instance* inst, const char* id)` | Soft-removes node. |
| `GSSK_DeactivateEdge` | `GSSK_Status GSSK_DeactivateEdge(GSSK_Instance* inst, const char* id)` | Soft-removes edge. |

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
