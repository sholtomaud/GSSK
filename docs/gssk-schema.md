# GSSK JSON Schema Reference — v2

> **Schema version: 2** — increment `schema_version` in `metadata` when breaking
> changes are made to this document. The formal machine-readable schema is
> [`gssk.schema.json`](../gssk.schema.json) at the project root.
> This document is the prose companion to that schema.

Every domain-specific serialiser (`GSSKModelSerializer`, `HouseholdSerializer`, …)
must produce JSON that validates against `gssk.schema.json`. The kernel will reject
non-conforming input with `GSSK_ERR_SCHEMA_VIOLATION`.

---

## Odum's Four-Position Inter-Block Array

This schema is grounded in Odum's EXTEND simulation framework
(*Modelling for All Scales*, 2000, Appendix A). Odum found that every connection
between blocks in an energy systems diagram carries four channels of information:

| Position | Odum's name | Description | GSSK mapping |
|---|---|---|---|
| 1 | **Code** | Type/identity of what is flowing | `edge.carrier` (free string) |
| 2 | **Force** | Driving potential causing the flow | Implicit: `logic` type + origin node Q |
| 3 | **Flow** | Quantity per unit time | Computed by `logic` + `params.k` |
| 4 | **Transformity** | Quality ratio (solar energy per unit) | Computed from `node.quality_input` |

Positions 2 and 3 were already encoded in the v1 schema via `logic` and `params`.
Positions 1 and 4 are new in v2 (`carrier` and `quality_input`/`output_mode`).

---

## Top-level structure

```json
{
  "metadata": { … },   // optional, but schema_version required for production
  "nodes":    [ … ],   // required — at least one node
  "edges":    [ … ],   // optional
  "config":   { … }    // optional — defaults documented below
}
```

---

## `metadata` object

| Field | Type | Required | Description |
|---|---|---|---|
| `schema_version` | integer | **yes** | Must be `2`. Kernel rejects other values. |
| `name` | string | no | Human-readable model name |
| `description` | string | no | Free-text description |
| `author` | string | no | Serialiser or author name |
| `created_at` | string (ISO 8601) | no | Generation timestamp |

```json
"metadata": {
  "schema_version": 2,
  "name": "Household Cash Flow",
  "author": "GSSKModelSerializer/2.0",
  "created_at": "2026-05-23T00:00:00Z"
}
```

> [!IMPORTANT]
> `schema_version: 2` is required. The kernel enforces this and rejects v1 models
> that omit or set a different version.

---

## `nodes` array

Each element is a compartment in the system. **The array index is the column index
in `GSSK_GetState()` — serialisers own this ordering contract.**

| Field | Type | Required | Description |
|---|---|---|---|
| `id` | string (1–63 chars) | **yes** | Unique. Stable across mutations. |
| `type` | string | **yes** | `"storage"`, `"source"`, `"sink"`, `"constant"` |
| `value` | number | **yes** | Initial Q at `t_start`, or fixed value for source/constant |
| `quality_input` | number ≥ 0 | no | **NEW v2** — boundary quality ratio (Odum Position 4). Enables quality accounting. |
| `output_mode` | string | no | **NEW v2** — `"partition"` (default) or `"replicate"`. |
| `visual` | object | no | UI layout hints. Kernel ignores. |

### Node types

| Type | dQ/dt computed? | Notes |
|---|---|---|
| `storage` | **yes** | Accumulates: bank balance, biomass, energy store |
| `source` | no (fixed) | External driver: salary, sunlight. Set `quality_input` here. |
| `sink` | no (fixed) | Drain: taxes, heat loss |
| `constant` | no (fixed) | Reference value for `interaction`/`limit` control nodes |

### `quality_input` — Odum's boundary transformity

Set on `source` or `constant` nodes to give the quality ratio at the system boundary.
Once any node has `quality_input > 0`, the quality accounting pass runs every step
and `GSSK_GetTransformationRatio()` returns non-null values.

Convention: use `1.0` for the ultimate solar energy boundary. Derived nodes
(wind, rain, chemical energy) get higher values reflecting their transformation depth.

### `output_mode` — Bastianoni split vs. co-product

Controls how quality propagates when this node fans out to multiple targets:

- `"partition"` (default): quality divides proportionally to flow — *Bastianoni split*.
  Use for most flows: salary splitting into rent + groceries.
- `"replicate"`: each downstream branch receives the **full** quality — *Bastianoni
  co-product*. Use when one process produces two inseparable outputs (e.g. solar
  energy simultaneously driving wind AND rain).

> [!NOTE]
> `output_mode` does **not** affect ODE integration. It only affects the quality
> accounting pass. The flow equation `dQ/dt = ΣF_in − ΣF_out` is always conservative.

---

## `edges` array

Directed flows between nodes. Each edge is Odum's full inter-block connector.

| Field | Type | Required | Description |
|---|---|---|---|
| `id` | string | no | Stable identifier — required for `GSSK_SetEdgeK()`, `GSSK_DeactivateEdge()`, `coupled_edge` references |
| `origin` | string | **yes** | Node id the flow leaves |
| `target` | string | **yes** | Node id the flow enters |
| `carrier` | string | no | **NEW v2** — Odum Position 1 (Code). What is flowing. |
| `logic` | string | **yes** | Flow calculation type |
| `params` | object | **yes** | Logic-specific parameters |
| `output_mode` | string | no | **NEW v2** — overrides node output_mode for this edge |
| `coupled_edge` | string\|null | no | **NEW v2** — paired counter-flow edge id |
| `visual` | object | no | UI hints |

### `carrier` — Odum's Code field

A free string identifying what substance or entity is flowing. Does not affect the
ODE. Use domain conventions:

| Carrier string | Domain | Direction convention |
|---|---|---|
| `"energy"` | Physics/ecology | Source → sink |
| `"material"` | Chemistry/ecology | Source → sink |
| `"money"` | Economics | **Opposite** to commodity — use `coupled_edge` |
| `"information"` | Biology/AI | Any direction |
| `"force"` | Mechanics | Any direction |
| Custom (e.g. `"H2O"`, `"GBP"`) | Domain-specific | As appropriate |

### `coupled_edge` — energy-money counter-flows

In a commodity transaction, money flows opposite to goods. Model this with two edges
paired via `coupled_edge`:

```json
{ "id": "labour_delivery", "origin": "worker",   "target": "employer",
  "carrier": "energy", "logic": "constant", "params": { "k": 40.0 },
  "coupled_edge": "wage_payment" },
{ "id": "wage_payment",    "origin": "employer",  "target": "worker",
  "carrier": "money",  "logic": "constant", "params": { "k": 5000.0 },
  "coupled_edge": "labour_delivery" }
```

### Edge logic types

| `logic` | Formula | Required params | IDC eligible? |
|---|---|---|---|
| `constant` | `F = k` | `k` | ✅ |
| `linear` | `F = k × Q_origin` | `k` | ✅ |
| `interaction` | `F = k × Q_origin × Q_control` | `k`, `control_node` | ✅ (Riccati duet) |
| `limit` | `F = kQ / (1 + Q/C)` | `k`, `control_node` | ❌ (RK4 fallback) |
| `threshold` | `F = k if Q > threshold, else 0` | `k`, `threshold` | ❌ (RK4 fallback) |

---

## `config` object

| Field | Type | Default | Description |
|---|---|---|---|
| `t_start` | number | `0.0` | Simulation start time |
| `t_end` | number | `100.0` | Simulation end time |
| `dt` | number | `0.1` | Time step (must be > 0) |
| `method` | string | `"auto"` | Integration method (see below) |
| `solver_tolerance` | number | `1e-6` | Max relative error before dual-solver degrades |

### Integration methods

| Method | Description |
|---|---|
| `"auto"` | **(Default)** Kernel selects IDC where all edges are constant/linear/interaction. Runs IDC and RK4 in parallel each step and cross-validates. If error < `solver_tolerance` → uses IDC result (`CONFIDENCE_HIGH`). If error ≥ tolerance → uses RK4 (`CONFIDENCE_DEGRADED`, cybernetic adjustments frozen). |
| `"rk4"` | Classic RK4. Recommended for models with limit/threshold edges. |
| `"euler"` | First-order Euler. For debugging or very stiff step requirements only. |
| `"incipient"` | Force IDC. Eligible edges use matrix-exponential; ineligible fall back silently to RK4. No dual cross-validation. |

> [!TIP]
> Leave `method` unset (or `"auto"`) for all production models. The kernel will use
> the most accurate solver available for your network topology and verify the result.

---

## Topology mutation — runtime node/edge addition

The full schema of a model does not need to be known at initialisation. New nodes
and edges can be added at runtime — for example, when a household purchases a car,
takes on a loan, or opens a new investment account.

### Pattern

```c
// Life event: household buys a car
GSSK_AddNode(inst,
  "{\"id\":\"car\",\"type\":\"storage\",\"value\":0.0}");

GSSK_AddEdge(inst,
  "{\"id\":\"car_loan\",\"origin\":\"account\",\"target\":\"car\","
  "\"carrier\":\"money\",\"logic\":\"constant\",\"params\":{\"k\":500.0}}");
```

After `GSSK_AddNode` / `GSSK_AddEdge`:
- The new node is appended to the end of the state vector.
- Existing column indices are unchanged — old node manifests remain valid.
- `GSSK_ReclassifyNetwork()` is called automatically.

### Deactivation (soft removal)

```c
GSSK_DeactivateEdge(inst, "car_loan");   // zeroes k, marks inactive
GSSK_DeactivateNode(inst, "car");        // zeroes all connected edges
```

Deactivated slots are retained in the arrays. Reactivate an edge via
`GSSK_SetEdgeK()` followed by `GSSK_ReclassifyNetwork()`.

---

## Quality accounting

When any `source` or `constant` node has `quality_input > 0`, the kernel runs
the quality accounting pass after every `GSSK_Step()`. This implements the
Brown (2025) dynamic matrix method: solving `(−Aᵀ)·Tr = b` via Gaussian
elimination, where `b[i] = quality_input[i]` for source nodes.

### API

```c
const double *tr = GSSK_GetTransformationRatio(inst); // Tr[i] per node
const double *qf = GSSK_GetQualityFlow(inst);          // Tr×flow per node
double        eq = GSSK_GetEdgeQualityFlow(inst, idx); // Tr×flow on edge idx
GSSK_SolverConfidence c = GSSK_GetSolverConfidence(inst);
```

`GSSK_GetTransformationRatio()` returns `NULL` if no `quality_input` is set.

---

## State vector contract

**`GSSK_GetState()[i]` = node at position `i` in the `nodes` array.**

This ordering is fixed at `GSSK_Init()` and extended (never reordered) by
`GSSK_AddNode()`. The serialiser must maintain a `nodeManifest`:

```swift
struct GSSKSerialiserOutput {
    let json: Data
    let nodeManifest: [Int: String]  // e.g. [0: "account", 1: "groceries"]
}
```

After `GSSK_AddNode()`, refresh the manifest via `GSSK_GetNodeID(inst, newIndex)`.

> [!CAUTION]
> Never infer column order from node type or alphabetical order. The only source of
> truth is the position in the `nodes` JSON array as emitted by the serialiser.

---

## Complete example — household cash flow (v2)

```json
{
  "metadata": { "schema_version": 2, "name": "Household Cash Flow" },
  "nodes": [
    { "id": "salary",    "type": "source",  "value": 5000.0,
      "carrier": "money", "quality_input": 100.0 },
    { "id": "account",   "type": "storage", "value": 1000.0 },
    { "id": "groceries", "type": "sink",    "value": 0.0 },
    { "id": "rent",      "type": "sink",    "value": 0.0 }
  ],
  "edges": [
    { "id": "salary_in",     "origin": "salary",  "target": "account",
      "carrier": "money", "logic": "constant", "params": { "k": 5000.0 } },
    { "id": "groceries_out", "origin": "account", "target": "groceries",
      "carrier": "money", "logic": "constant", "params": { "k": 800.0 } },
    { "id": "rent_out",      "origin": "account", "target": "rent",
      "carrier": "money", "logic": "constant", "params": { "k": 1500.0 } }
  ],
  "config": { "t_start": 0.0, "t_end": 12.0, "dt": 1.0, "method": "auto" }
}
```

---

## Known limitations

| Limitation | Workaround |
|---|---|
| Node ID max 63 chars | Truncate or hash UUIDs |
| No native pulsed sources | Constant source + threshold gate |
| `limit`/`threshold` edges are not IDC-eligible | Kernel auto-falls back to RK4 |
| IDC uses order-6 Taylor approximation for matrix-exp | Adequate for typical dt; reduce dt if CONFIDENCE_DEGRADED |
| Topology deactivation is soft (slots not freed) | Acceptable for typical model sizes |
