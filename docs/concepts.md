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

Odum identifies seven fundamental symbol types. GSSK currently implements three natively; the remainder are on the Phase 7 roadmap.

| Odum symbol | Name | GSSK status | GSSK `type` |
|---|---|---|---|
| Circle with arrow | **Source** — outside source of inflows | ✓ implemented | `"source"` |
| Closed tank | **Storage** — accumulates state Q | ✓ implemented | `"storage"` |
| Ground symbol | **Heat Sink** — pathway of used energy | ✓ implemented | `"sink"` |
| Arrowhead (×) | **Interaction** — production process, work gate | ⚠ Phase 7 | `"interaction"` *(planned)* |
| Triangle | **Constant Gain Amplifier** — output ∝ control input | ⚠ Phase 7 | `"gain"` *(planned)* |
| D-shape | **Loop-Limited Converter** — Michaelis-Menten recycling | ⚠ Phase 7 | `"loop_limited"` *(planned)* |
| Diamond | **Exchange** — couples two carrier flows via price | ⚠ Phase 7 | `"exchange"` *(planned)* |
| Hourglass | **Switch / Digital Box** — on/off threshold process | ⚠ Phase 7 | `"switch"` *(planned)* |

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

| Odum symbol | Name | Composition | GSSK status |
|---|---|---|---|
| Rounded rectangle | **Producer** | `storage` + autocatalytic `interaction` + `sink` | ⚠ Phase 8 |
| Hexagon | **Consumer** | `storage` + multiple `interaction` inputs + `sink` | ⚠ Phase 8 |
| Dashed rectangle | **System / Sub-system Frame** | Encapsulation boundary over a named subgraph | ⚠ Phase 8 |
| Plain rectangle | **Miscellaneous Box** | Generic unspecified processing unit | ⚠ Phase 8 |
| Hourglass group | **Switching Box** | `switch` + internal subgraph with named control | ⚠ Phase 8 |

Phase 8 will introduce:
1. Built-in composite types expanding at `GSSK_Init` time.
2. User-defined archetypes in the model JSON (`"archetypes"` block).
3. Runtime pattern discovery — self-stabilising motifs proposed as new archetypes (Phase 9, see below).

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

Giannantoni's deeper claim is that systems do not merely *transform* — they *originate* new qualities. Structural patterns that recur and self-stabilise in a live graph are candidates for higher-order composite types. Phase 9 will implement a motif-detection layer that operationalises this principle computationally: detecting recurring subgraph patterns, proposing them as named archetypes, and computing a *generativity index* G(t) that measures the rate at which new stable patterns emerge.

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

The JSON model must include `"schema_version": 3` in `metadata`. Older versions are rejected. Phase 7 will introduce `schema_version: 4` with the corrected node type taxonomy; a migration tool will convert v3 models automatically.

---

*References:*
- *Odum, H.T. & Odum, E.C. (2000). Modeling for All Scales. Academic Press.*
- *Giannantoni, C. (2006). Mathematics for Generative Processes. Journal of Mathematical Analysis and Applications.*
- *Giannantoni, C. (2023). Generativity of Self-Organizing Processes. (Preprint.)*
- *Bastianoni, S. et al. (2011). Emergy and emergy algebra explained by means of ingenuous set theory. Ecological Modelling.*
