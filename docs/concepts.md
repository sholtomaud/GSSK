# Concepts

GSSK (General Systems Simulation Kernel) implements a numerical engine for Odum's **Energy Systems Language (ESL)** — a graphical language for modelling flows of energy, matter, money, and information in complex systems.

## Energy Systems Language

ESL models energy flows as a directed graph of **nodes** (storages, sources, sinks) and **edges** (flows). Each edge carries a flow rate Q determined by the state of its origin node and a transfer coefficient *k*.

| Symbol | Type | Behaviour |
|--------|------|-----------|
| Tank | `storage` | Accumulates inflow minus outflow |
| Source | `source` | Infinite reservoir; holds constant state |
| Sink | `sink` | Drain; accepts flow without limit |

## Node Types

### Storage
A storage node holds state `Q` and obeys:

```
dQ/dt = ∑ inflows − ∑ outflows
```

Initial state is set by the `value` field in the JSON model.

### Source
A source node has a fixed state equal to its `value`. It never changes and can drive unlimited outflow.

### Sink
A sink accepts all inflow; its state accumulates but the solver never uses it as a driver.

## Edge Logic Types

| Logic | Formula | Parameters |
|-------|---------|-----------|
| `linear` | `f = k · Q_origin` | `k` |
| `constant` | `f = k` | `k` |
| `michaelis_menten` | `f = k · Q_origin / (Km + Q_origin)` | `k`, `Km` |
| `limit` | `f = k · Q_origin / (C + Q_origin)` | `k`, `C` (saturation) |
| `interaction` | `f = k · Q_origin · Q_control` | `k`, `control_node` |

## Integration Methods

The solver advances time by numerical integration. Two methods are available:

| Method | Flag | Accuracy | Cost |
|--------|------|----------|------|
| Forward Euler | `"euler"` | O(h) | 1 eval/step |
| Runge-Kutta 4 | `"rk4"` | O(h⁴) | 4 evals/step |

RK4 is the default and recommended for all but the simplest models.

### Adaptive Stepping

`GSSK_StepAdaptive` implements a step-doubling error estimator. It halves the step when the relative error exceeds the tolerance `ε = 1e-6` and doubles it when error is small, subject to a maximum of `10 × dt_default`.

## Conservation and Carriers

A **carrier** identifies the physical substance flowing through a sub-network (money, energy, mass, information). Carriers marked `conserved: true` are subject to conservation checking: the kernel computes the per-carrier conservation error as the absolute difference between total inflow and total outflow at each time step.

```json
"carriers": [
  { "id": "money",  "unit": "AUD", "conserved": true  },
  { "id": "energy", "unit": "kWh", "conserved": true  }
]
```

Nodes and edges reference their carrier via the `"carrier"` field. Edges without a carrier belong to the default (legacy) carrier.

## Forward Sensitivity Analysis

The kernel supports forward (tangent-linear) sensitivity to edge parameters. Enabling sensitivity for edge `i` computes `∂Q_j / ∂k_i` for all state nodes `j` alongside the primal integration. Sensitivities are initialised to zero and accumulate from the first step.

## Mutation Log

Every structural change (add node, add edge, reclassify, deactivate) is recorded in an append-only mutation log. The log can be exported as a JSON array and replayed via `GSSK_Replay` to reconstruct model history. Use `GSSK_SetMutationCause` to attach a human-readable reason to the next mutation.

## Schema Versioning

The JSON model must include `"schema_version": 3` in `metadata`. Older versions are rejected with `ERR_UNSUPPORTED_SCHEMA`. The kernel version string follows semantic versioning (`major.minor.patch`) and is embedded at build time.
