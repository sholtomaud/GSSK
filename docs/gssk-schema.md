# GSSK JSON Schema Reference

> Version: **1** — increment `schema_version` in `metadata` when breaking changes are made to this document.

This document is the authoritative contract between the GSSK kernel and any serialiser (Swift, Python, JS, etc.). Every domain-specific serialiser (`GSSKModelSerializer`, `HouseholdSerializer`, …) must produce JSON that conforms to this spec. The kernel will reject non-conforming input with a `GSSK_ERR_SCHEMA_VIOLATION` error.

---

## Top-level structure

```json
{
  "metadata": { … },   // optional but strongly recommended
  "nodes":    [ … ],   // required — at least one node
  "edges":    [ … ],   // optional — omit for a single isolated storage
  "config":   { … }    // optional — defaults are documented below
}
```

---

## `metadata` object *(optional, but version tag is required for production)*

| Field | Type | Required | Description |
|---|---|---|---|
| `schema_version` | integer | **yes** | Schema version. Kernel must reject if it doesn't support this version. Currently `1`. |
| `name` | string | no | Human-readable model name |
| `description` | string | no | Free-text description |
| `author` | string | no | Origin / serialiser name |
| `created_at` | string (ISO 8601) | no | Generation timestamp |

```json
"metadata": {
  "schema_version": 1,
  "name": "Household Cash Flow",
  "author": "GSSKModelSerializer/2.0",
  "created_at": "2026-05-21T05:00:00Z"
}
```

> [!IMPORTANT]
> `schema_version` is the versioning hook the Swift developer requested. The kernel currently accepts any integer here (it does not yet enforce a version check), but serialisers **must** emit it so that a future kernel version can reject incompatible models cleanly.

---

## `nodes` array *(required)*

Each element represents a compartment in the system. **The order of nodes in this array is the column order of the state vector returned by `GSSK_GetState()`.**

| Field | Type | Required | Allowed values |
|---|---|---|---|
| `id` | string (≥ 1 char) | **yes** | Must be unique across the model. Max 63 chars (C internal limit). |
| `type` | string | **yes** | `"storage"`, `"source"`, `"sink"`, `"constant"` |
| `value` | number | **yes** | Initial value of Q at `t_start` |
| `visual` | object | no | Layout hints for UI (ignored by kernel) |

### Node types

| Type | dQ/dt computed? | Typical use |
|---|---|---|
| `storage` | **yes** | Anything that accumulates: bank balance, inventory, energy store |
| `source` | **no** (fixed) | External inflow driver: salary, sunlight, revenue stream |
| `sink` | **no** (fixed) | Drain with no recovery: taxes paid, energy lost to heat |
| `constant` | **no** (fixed) | Fixed reference value used as a `control_node` in interactions |

> [!NOTE]
> `source` and `constant` nodes are **not** depleted by outflows. Their Q value stays at the declared `value` throughout the simulation. If you want a finite source that depletes, model it as `storage`.

### How to represent inflows

| Inflow pattern | Recommended modelling |
|---|---|
| Constant periodic income (salary) | `source` node with `value: <monthly_amount>` + `constant` edge (k = 1.0) into a `storage` node |
| Income proportional to a balance | `storage` origin + `linear` edge |
| One-off lump sum | Set the target `storage` node's initial `value` to the lump sum |
| Pulsed / irregular income | Not natively supported — approximate as a constant `source` or use the `threshold` logic to gate flows |

---

## `edges` array *(optional)*

Each element is a directed flow between two nodes. Multi-hop chains (`salary → account → groceries`) are modelled as **two edges**:

```
salary  →(edge A)→  account  →(edge B)→  groceries
```

The kernel sums all edges simultaneously each step — there is no execution ordering issue.

| Field | Type | Required | Description |
|---|---|---|---|
| `id` | string | no | Stable identifier for this flow (useful for `GSSK_SetEdgeK`) |
| `origin` | string | **yes** | Node `id` that the flow leaves |
| `target` | string | **yes** | Node `id` that the flow enters |
| `logic` | string | **yes** | Flow calculation type (see below) |
| `params` | object | **yes** | Logic-specific parameters |
| `visual` | object | no | UI layout hints (ignored by kernel) |

### Edge logic types

| `logic` | Flow formula | Required params | Optional params |
|---|---|---|---|
| `constant` | `flow = k` | `k` | — |
| `linear` | `flow = k × Q_origin` | `k` | — |
| `interaction` | `flow = k × Q_origin × Q_control` | `k`, `control_node` | — |
| `limit` | `flow = (k × Q_origin) / (1 + Q_origin / Q_control)` | `k`, `control_node` | — |
| `threshold` | `flow = k if Q_origin > threshold, else 0` | `k`, `threshold` | — |

### `params` object

| Field | Type | When required |
|---|---|---|
| `k` | number | **always** — scaling coefficient |
| `control_node` | string (node id) | `interaction`, `limit` — **required** |
| `threshold` | number | `threshold` — defaults to `0.0` if omitted |

---

## `config` object *(optional — defaults shown)*

| Field | Type | Default | Description |
|---|---|---|---|
| `t_start` | number | `0.0` | Simulation start time |
| `t_end` | number | `100.0` | Simulation end time (must be > `t_start`) |
| `dt` | number | `0.1` | Time step (must be > 0) |
| `method` | string | `"euler"` | Integration method: `"euler"` or `"rk4"` |

> [!TIP]
> Use `"rk4"` for financial/biological models — it is fourth-order accurate and far more stable than Euler for stiff systems. The performance cost on modern hardware is negligible.

---

## State vector contract

This is the most critical section for any serialiser.

**`GSSK_GetState()` returns a `double*` whose index `i` corresponds exactly to the node at position `i` in the `nodes` array.**

```
nodes[0] → state[0]
nodes[1] → state[1]
…
nodes[N-1] → state[N-1]
```

This ordering is set at `GSSK_Init` time and never changes. **The serialiser owns this ordering.** It must emit a manifest alongside the JSON so the result mapper can recover node identity from column index:

```swift
struct GSSKSerialiserOutput {
    let json: Data
    /// Maps state-vector column index → stable domain node ID.
    /// Column ordering matches the `nodes` array order in `json`.
    let nodeManifest: [Int: String]  // e.g. [0: "account_abc123", 1: "groceries_def456"]
}
```

After `GSSK_Init`, the serialiser can also recover the manifest from the live kernel via `GSSK_GetNodeID(inst, index)` without storing it separately.

> [!CAUTION]
> Never infer column order from node type or alphabetical order. The only source of truth is the position in the `nodes` JSON array as emitted by the serialiser.

---

## Complete worked example — household cash flow

This models a salary flowing into a current account, which drains into a groceries spend bucket and a rent payment.

```json
{
  "metadata": {
    "schema_version": 1,
    "name": "Household Cash Flow",
    "author": "GSSKModelSerializer/1.0"
  },
  "nodes": [
    { "id": "salary",    "type": "source",  "value": 5000.0 },
    { "id": "account",   "type": "storage", "value": 1000.0 },
    { "id": "groceries", "type": "sink",    "value": 0.0    },
    { "id": "rent",      "type": "sink",    "value": 0.0    }
  ],
  "edges": [
    {
      "id": "salary_in",
      "origin": "salary", "target": "account",
      "logic": "constant",
      "params": { "k": 5000.0 }
    },
    {
      "id": "groceries_out",
      "origin": "account", "target": "groceries",
      "logic": "constant",
      "params": { "k": 800.0 }
    },
    {
      "id": "rent_out",
      "origin": "account", "target": "rent",
      "logic": "constant",
      "params": { "k": 1500.0 }
    }
  ],
  "config": {
    "t_start": 0.0,
    "t_end": 12.0,
    "dt": 1.0,
    "method": "euler"
  }
}
```

**State vector columns for this model:**

| Column | Node ID | Type |
|---|---|---|
| 0 | `salary` | source (fixed) |
| 1 | `account` | storage (evolves) |
| 2 | `groceries` | sink (fixed dQ, accumulates inflow) |
| 3 | `rent` | sink (fixed dQ, accumulates inflow) |

**Node manifest (serialiser output):**
```swift
nodeManifest: [0: "salary", 1: "account", 2: "groceries", 3: "rent"]
```

---

## Adding a new domain type

Each new domain (business, rental property, investment portfolio) needs only a conforming serialiser:

1. Map domain entities to nodes (storage = accounts/stocks, source = income, sink = expenses)
2. Map domain relationships to edges (choose the logic type that fits the flow)
3. Emit `metadata.schema_version: 1`
4. Return a `GSSKSerialiserOutput` with the `nodeManifest`

**No kernel changes are required.**

---

## Known limitations & future work

| Limitation | Workaround |
|---|---|
| Node ID max 63 chars | Truncate or hash UUIDs before embedding as `id` |
| No native pulsed/periodic sources | Model as constant source + threshold gate |
| No time-varying `k` | Call `GSSK_SetEdgeK()` between steps to update coefficients |
| No branching logic (if/else flows) | Use two `threshold` edges with complementary thresholds |
| `schema_version` not enforced by kernel v1 | Enforced by serialiser convention; kernel enforcement planned for v2 |
