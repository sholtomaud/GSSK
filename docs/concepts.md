# Concepts

GSSK implements a numerical engine for Odum's **Energy Systems Language (ESL)** — a graphical diagramming language for modelling flows of energy, matter, money, and information through complex systems (Odum & Odum, 2000).

---

## The ESL Topology Rule

In ESL the distinction between nodes and edges is not arbitrary:

> **Edges are flows only** — pathways carrying energy, matter, money, information, or code from one unit to another.
>
> **Nodes are everything else** — processing units, storages, sources, sinks, gates, amplifiers, converters, and transaction points. Even when a node has no internal dynamics beyond a fixed coefficient *k*, that coefficient is the *state* of the node — it is a property of the unit, not of the flow passing through it.

This is analogous to the distinction in dimensional analysis between *fundamental* dimensions (length, time, mass) and *derived* dimensions composed from them.

---

## Fundamental Node Types (Odum Fig 1.2a)

Odum identifies seven fundamental symbol types. All are implemented, along with a `constant` type that has no Odum symbol.

| Odum symbol | Name | GSSK status | GSSK `type` |
|---|---|---|---|
| Circle with arrow | **Source** — outside source of inflows | ✓ implemented | `"source"` |
| Closed tank | **Storage** — accumulates state Q | ✓ implemented | `"storage"` |
| Ground symbol | **Heat Sink** — pathway of used energy | ✓ implemented | `"sink"` |
| Arrowhead (×) | **Interaction** — production process, work gate | ✓ implemented | `"interaction"` |
| Triangle | **Constant Gain Amplifier** — output ∝ control input | ✓ implemented | `"gain"` |
| D-shape | **Loop-Limited Converter** — Michaelis-Menten recycling | ✓ implemented | `"loop_limited"` |
| Diamond | **Exchange** — couples two carrier flows via price | ✓ implemented | `"exchange"` |
| Hourglass | **Switch / Digital Box** — on/off threshold process | ✓ implemented | `"switch"` |
| *(none)* | **Constant** — fixed reference value | ✓ implemented | `"constant"` |

The five processing types (`interaction`, `gain`, `loop_limited`, `exchange`, `switch`) are configured through the node's `params` block — `k`, `C`, `threshold`, `price` — rather than through edge parameters. An edge touching a processing node may therefore omit `logic` and `params.k`, which default to `"linear"` and `1.0`; every other edge must supply both.

An unrecognised `type` string is **rejected**. `GSSK_Init` returns `GSSK_ERR_SCHEMA_VIOLATION` naming the node and the offending string — `Schema Error: Node 'grasss' has unknown type 'storge'.` — so a typo fails at load rather than becoming a silent `storage` node. Valid strings are the nine primitives above, a built-in composite, or an archetype the model declares itself.

### Implemented node types

#### Source
Fixed state equal to `value`. Never changes; drives unlimited outflow. Represents an outside reservoir — sunlight, salary, market supply.

#### Storage
Accumulates inflow minus outflow:
```
dQ/dt = ∑ inflows − ∑ outflows
```
Initial state set by `value`. This is the canonical ODU tank symbol.

#### Sink (Heat Sink)
Accepts all inflow unconditionally. State accumulates but is never used as a driver. Represents irreversible dispersal — heat loss, waste landfill, tax.

### Planned fundamental node types (Phase 7)

#### Interaction (Production Process)
Two or more incoming flow edges; output proportional to the *product* of the input forces. The coefficient `k` is the internal state of the interaction node — it encodes the binding efficiency of the production process (Odum Fig 2.6).

```
F_out = k × Q_A × Q_B
```

> **Current workaround:** modelled as `logic: "interaction"` on an edge, with `control_node` pointing to the second input. This misrepresents the topology — `k` belongs to a node, not a flow.

#### Constant Gain Amplifier
A control input provides a small signal; a separate energy source supplies the power for amplification. Output is proportional to the control input with gain `k` (Odum Fig 2.7). Example: a microphone signal controlling a power amplifier; reproduction where offspring number is the gain.

```
F_out = k × Q_control   (energy source assumed non-limiting)
```

#### Loop-Limited Converter (Michaelis-Menten)
Output saturates due to an internal recycling cycle of limiting material. Named the *Michaelis-Menten module* by Odum, who notes it was discovered in 1913 and applies wherever an enzyme or catalyst is recycled internally (Odum Fig 2.8). Appropriate for photosynthesis, enzyme kinetics, any process where a catalyst is the bottleneck.

```
F_out = k × Q_in / (1 + Q_in / C)
```

where `C` is the amount of limiting material in the internal cycle.

#### Exchange
Couples two carrier flows through a price — typically money (dashed `$`) flowing one way and goods or services flowing the other (Odum Fig 2.4). The exchange node holds a `price` parameter; per step it debits `price × F` from the money carrier and credits `F` to the goods carrier atomically. Variants: purchase transaction, barter, market-price-setting exchange.

Required for modelling monetary economies; the household model grocery transaction should use this node.

#### Switch / Digital Box
An on/off process node controlled by one or more threshold conditions (Odum Fig 2.12). Unlike continuous-range nodes, the switch is *digital* — flow is either fully on or fully off depending on whether the controlling storage exceeds a threshold. Variants include overflow switches, flip-flops with high and low thresholds, and externally controlled on/off gates.

---

## Composite Node Types (Odum Fig 1.2b)

Odum's *composite symbols* are aggregates — each is a named combination of fundamental nodes and flows that recurs frequently enough to warrant its own shorthand. Like derived dimensions, they decompose into fundamentals.

Composites are **shipped** as of Phase 8. A composite node is written like any other node — `{"id": "plant", "type": "producer", "value": 50.0}` — and is expanded into its constituent primitives at `GSSK_Init` time. Nothing composite survives into the solver; by the time you can step the model, only fundamentals exist.

| Odum symbol | `type` | Expands to | GSSK status |
|---|---|---|---|
| Rounded rectangle | `producer` | `body` (storage) + `gate` (interaction, autocatalytic) + `heat` (sink) | ✅ shipped |
| Hexagon | `consumer` | `body` (storage) + `heat` (sink) | ✅ shipped |
| Plain rectangle | `misc_box` | `box` (storage) | ✅ shipped |
| Dashed rectangle | `system_frame` | *(nothing)* — recorded as a single `constant` node | ⚠ partial |
| Hourglass group | **Switching Box** | `switch` + internal subgraph with named control | ❌ not implemented |

`system_frame` is structural only: it reserves the name but expands to no subgraph, so it cannot yet encapsulate a named set of nodes. Treat it as a placeholder.

### Expansion and naming

Each expanded member is named `{instance_id}__{template_node_id}`, and each internal edge `{instance_id}__{template_edge_id}`. Both halves truncate to 29 characters. So `plant` above becomes three state nodes — `plant__body`, `plant__gate`, `plant__heat` — wired by four internal edges `plant__feed_a`, `plant__feed_b`, `plant__prod`, `plant__resp`.

Two consequences worth internalising:

- **Node indices are no longer positional.** Without composites, the *n*th entry of the model's `nodes` array is column *n* of `GSSK_GetState()`. Once any composite is present that correspondence breaks. Resolve indices with `GSSK_FindNodeIdx()`.
- **Never infer membership from the id.** A node declared directly may legitimately contain `__`, and a composite id containing `__` cannot be split unambiguously. Use the membership API below.

An edge naming a composite instance as an endpoint resolves to that composite's declared port: `{"origin": "sun", "target": "plant"}` attaches to `plant`'s `in` port (`body`), not to some aggregate.

### Built-in parameter overrides

The instance's `params` block tunes internal conductances:

| `type` | Parameter | Overrides | Default |
|---|---|---|---|
| `producer` | `k_production` | gain of the internal `gate` node | 0.01 |
| `producer` | `k_respiration` | `body → heat` edge (`resp`) | 0.05 |
| `consumer` | `k_metabolism` | `body → heat` edge (`metab`) | 0.1 |

### User-defined archetypes

A top-level `"archetypes"` block registers your own templates; the key becomes a usable `type`. Ports define external attachment — without an `in`/`out` port the composite cannot be wired to the rest of the model, and the instance's `value` is not applied to any member.

```jsonc
{
  "metadata": { "schema_version": 4 },
  "archetypes": {
    "self_limiter": {
      "nodes": [ { "id": "a", "type": "storage", "value": 5 },
                 { "id": "b", "type": "storage", "value": 0 } ],
      "edges": [ { "id": "ab", "origin": "a", "target": "b", "logic": "linear", "params": { "k": 0.2 } },
                 { "id": "ba", "origin": "b", "target": "a", "logic": "linear", "params": { "k": 0.1 } } ],
      "ports": { "in": "a", "out": "b" }
    }
  },
  "nodes": [ { "id": "lim", "type": "self_limiter", "value": 5.0 } ]
}
```

Limits: 32 archetypes (4 built-ins included), 16 nodes and 32 edges per archetype, 128 composite instances per model. Built-ins are matched first, so a user archetype cannot shadow one. A `type` naming neither a primitive nor a declared archetype is **an error** — archetypes are parsed before nodes, so by the time the node loop runs the parser knows every name that was declared and can tell a typo from a legitimate reference.

### Querying the expansion

Membership is recorded during expansion and exposed in both directions, so consumers never parse ids:

| Function | Returns |
|---|---|
| `GSSK_GetCompositeCount` / `GSSK_GetCompositeID` | enumerate composite instances |
| `GSSK_GetCompositeArchetype` | which archetype an instance came from |
| `GSSK_GetNodeComposite` | owning instance id, or `""` if declared directly |
| `GSSK_GetNodeRole` | member's role in the template (`body`, `gate`, `heat`) |
| `GSSK_GetCompositeMemberCount` / `GSSK_GetCompositeMemberIndex` | iterate an instance's members |
| `GSSK_GetArchetypeCount` / `GSSK_GetArchetypeName` | enumerate registered archetypes |

`GSSK_GetNodeRole` is what you want to aggregate "the storage of every producer" — it is stable across renamings of the instance, which string-matching is not.

Runtime pattern discovery — self-stabilising motifs proposed as new archetypes — is Phase 9, see below.

---

## Current Edge Flow Types

While GSSK's node type taxonomy is being corrected in Phase 7, the current `logic` field on edges encodes flow behaviour. This is a pragmatic simplification that will be replaced by proper node types. The mapping is:

| Current `edge.logic` | Will become (Phase 7) | Odum symbol |
|---|---|---|
| `"linear"` | `storage` → flow edge with `k` | linear pathway |
| `"constant"` | `source` → flow edge with `k` | constant pathway |
| `"interaction"` | `type: "interaction"` node | Interaction / Work Gate |
| `"limit"` | `type: "loop_limited"` node | Loop-Limited Converter |
| `"threshold"` | `type: "switch"` node | Switch / Digital Box |

---

## Carriers

A **carrier** identifies the physical substance flowing through a sub-network. Carriers separate the graph into domains that can be independently conserved.

```json
"carriers": [
  { "id": "money",       "unit": "AUD",             "conserved": true  },
  { "id": "energy",      "unit": "kWh",             "conserved": true  },
  { "id": "material",    "unit": "kg",              "conserved": true  },
  { "id": "information", "unit": "decisions/month", "conserved": false }
]
```

Carriers marked `conserved: true` are subject to per-carrier conservation checking at each step via `GSSK_GetCarrierConservationError`. Information is typically `conserved: false` — attention and decisions are dissipated, not physically conserved.

Cross-carrier coupling is natural in ESL — an Interaction node can take inputs from different carriers (e.g. bank balance controlling material flow rate), representing Odum's *work gate* coupling money to matter.

---

## Emergy, Transformity, and Empower

**Emergy** (spelled with an *m*) is the total energy of one type, directly and indirectly required to produce a product or service (Odum, 1996). It is measured in *solar emjoules* (sej).

**Transformity** (Tr) is the emergy per unit energy — a quality factor expressing how much solar work was required to produce one joule of a given form. High-transformity flows carry more information or structure per unit energy.

**Empower** is emergy flow per unit time.

GSSK computes transformity propagation automatically at each step. Nodes with `quality_input` set (source nodes) inject their transformity into the flow network; storage and processing nodes accumulate quality-weighted inflows.

```c
GSSK_GetTransformationRatio(inst, node_idx)   // Tr for node
GSSK_GetQualityFlow(inst, node_idx)            // empower at node
GSSK_GetEdgeQualityFlow(inst, edge_idx)        // empower on edge
```

Transformity sensitivity (`GSSK_GetTransformitySensitivity`) answers: if I change edge coefficient `k_j`, how does the transformity of node `i` change? This is the natural question for emergy-based optimisation.

---

## Giannantoni's Incipient Differential Calculus (IDC)

Conventional ODE solvers (Euler, RK4) treat the equations of a system as pre-given and integrate forward. Giannantoni's **Incipient Differential Calculus** (IDC) reconceives integration as a generative process: the solution is not computed step-by-step but recognised as an *originating* structure whose qualitative character can be captured exactly for certain primitive forms (Giannantoni 2006, 2023).

For GSSK this has a practical payoff. Interaction flows of the form `F = k·Q_A·Q_B` (the coupled-logistic, or Riccati, system) have an exact closed-form IDC solution:

```
Q_A(t) = S · Q_A₀ / (Q_A₀ + Q_B₀ · exp(k · S · dt))
```

where `S = Q_A₀ + Q_B₀` (conserved sum). This is not an approximation — it is the exact trajectory. GSSK detects isolated two-node interaction systems and uses this path automatically, bypassing RK4 entirely.

For longer chains and cyclic interaction networks, GSSK applies a Padé-(3,3) matrix exponential, which is A-stable and captures the qualitative character of the decay without the step-size restrictions of explicit methods.

### Generativity (Phase 9)

Giannantoni's deeper claim is that systems do not merely *transform* — they *originate* new qualities. Structural patterns that recur and self-stabilise in a live graph are candidates for higher-order composite types.

Phase 9 is **shipped**. After each `GSSK_Step` the kernel scans the live graph for recurring connected subgraph motifs of 2–3 nodes, identified by node-type composition and directed connectivity. A motif seen at least 3 times per step for 10 consecutive steps becomes a *candidate* — inspect the table with `GSSK_GetMotifCount`, `GSSK_GetMotifCanon`, `GSSK_GetMotifOccurrence`, `GSSK_GetMotifStableSteps` and `GSSK_IsMotifCandidate`. `GSSK_ProposeArchetype` promotes a candidate to a named archetype registered in the running instance, which is then usable as a node `type` exactly like a user-declared one. `GSSK_GetGenerativityIndex` reports G(t), the rate at which new stable patterns emerge.

The scan is skipped for graphs above 64 nodes, so on large models the motif table stays empty by design.

---

## Integration Methods

| Method | Config value | Notes |
|---|---|---|
| IDC (auto) | `"auto"` | Default. Exact for isolated duets; Padé-3,3 otherwise. |
| Runge-Kutta 4 | `"rk4"` | 4th-order explicit. Use for debugging or comparison. |
| Euler | `"euler"` | 1st-order. Not recommended for production. |
| Adaptive (DOPRI5) | `"adaptive"` | Dormand-Prince 5(4) with PI step-size control. |

### Solver Confidence

After each step, `GSSK_GetSolverConfidence` returns `HIGH` (IDC path used, per-step error below tolerance) or `DEGRADED` (IDC/RK4 disagreement detected or conservation violation). Use `GSSK_GetStepErrorEstimate` for a scalar bound on the per-step error.

---

## Sensitivity Analysis

GSSK supports three sensitivity modes:

**Forward (tangent-linear):** computes `∂Q_j/∂k_i` for all state nodes `j` alongside the primal run. Cost is O(n × m) per step where m is the number of tracked parameters.

**Adjoint:** cheaper when outputs << parameters. Runs a backward integration after the forward trajectory to compute parameter gradients for a scalar objective.

**Transformity sensitivity:** `∂Tr_i/∂k_j` — how does changing a flow coefficient affect the emergy quality of a downstream node? Derived via the implicit-function theorem on the transformity propagation system.

---

## Mutation Log

Every structural change (add node, add edge, deactivate, reclassify, set k) is appended to an in-memory mutation log with fields `{t, op, target_id, payload, cause}`. The log is serialised in snapshots and replayed exactly by `GSSK_Replay`. Use `GSSK_SetMutationCause` to attach a reason (`"user"`, `"calibration"`, `"event:<id>"`) to the next operation.

---

## Schema Versioning

The JSON model declares `"schema_version"` in `metadata`. The kernel accepts **2, 3 or 4**; any other value is rejected with an error naming the supported set. A v2 model is auto-migrated to v3 at init with a warning. When the `metadata` block is absent entirely, the version defaults to 3.

| Version | Adds |
|---|---|
| 2 | baseline; auto-migrates to 3 |
| 3 | `snapshot`, `mutation_log`, `carriers`, metadata versioning |
| 4 | `archetypes` block and composite node types |

`GSSK_GetSchemaVersion` reports the version of the loaded model. The CLI migrates in place: `gssk migrate --from 2 in.json out.json` and `gssk migrate --from 3 in.json out.json`.

The published `gssk.schema.json` validates the **pre-expansion** surface — the document you hand to `GSSK_Init`. It is not the post-expansion vocabulary: after init the node set is larger and can contain `interaction` nodes no model declared. Documents emitted by `GSSK_SerializeSnapshot` validate against the same schema, but their `nodes` array is the expanded one.

---

*References:*
- *Odum, H.T. & Odum, E.C. (2000). Modeling for All Scales. Academic Press.*
- *Giannantoni, C. (2006). Mathematics for Generative Processes. Journal of Mathematical Analysis and Applications.*
- *Giannantoni, C. (2023). Generativity of Self-Organizing Processes. (Preprint.)*
- *Bastianoni, S. et al. (2011). Emergy and emergy algebra explained by means of ingenuous set theory. Ecological Modelling.*
