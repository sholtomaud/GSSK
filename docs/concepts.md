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
| `"ratio"` | — (no Odum symbol; see below) | division |
| `"reversible"` | — (an edge property, not a node) | barb-less pathway |

### `reversible` — the barb-less pathway (GIP-0001 G3)

`F = k × (Q_origin − Q_target)`, **signed**.

Odum draws two pathway kinds, and the distinction is the notation itself:

> Where the flow depends only on the force behind it, an arrowhead (barb) is used… Where the flow depends on the difference between the force at one end and the back force from the other end, a line is used without a barb, and this pathway may flow in either direction.
>
> — *Modeling for All Scales*, p.23

Every other logic computes forward from origin quantities and never reads the target. `reversible` is the only one that reads both ends, which makes it the only one that can transport **backwards along its declared direction** — and for a barb-less line that is not an error. `origin` and `target` stop meaning "from" and "to" and start meaning only "the first end" and "the second end". Swapping them produces an identical trajectory.

Use it for diffusion, exchange across a gradient, and any equilibrating process. Two stores joined by one reversible edge converge to equal quantity from any initial condition, and their total is conserved exactly, because `F` is subtracted at one end and added at the other in the same statement.

**It is not two opposed `linear` edges.** `A→B` with `k₁` plus `B→A` with `k₂` gives `k₁·Q_A − k₂·Q_B`, which equals the gradient form only when the two conductances happen to be equal — nothing enforces that, and a model whose conductances have drifted equilibrates to `Q_A/Q_B = k₂/k₁` instead of to equality, silently. It also *draws* as two barbed pathways, which in Odum's notation means a different thing (the counter-current; see the `exchange` node). Nor is it `linear` with a negative `k`, which still never reads the target.

**It is exactly integrable.** Being linear in the state, the incipient/IDC solver integrates it exactly rather than linearising about the current operating point — unlike `limit` (linearised Michaelis-Menten) and `ratio` (exact only in the numerator, for a frozen denominator). It is the first logic whose flow-matrix contribution touches four entries rather than two, because the flow depends on both ends.

**Quality accounting attributes nothing to a backward flow.** A flow running back up a gradient is not producing the node it arrives at, so `GSSK_GetEdgeQualityFlow` reads `0.0` while the pathway reverses and resumes when the gradient does. `GSSK_GetFlows` reports the signed rate and is the right call for the physics. See [ADR 0007](adr/0007-reversible-pathway.md).

### `ratio` — division (Phase C.1, extended in C.3)

`F = k × Q_numerator / max(Q_control, ε)`

Division has no Odum symbol of its own, and it exists here because Odum's claim that price is circulating money over real work delivered — `P = M/W` — cannot otherwise be written down. See [ADR 0002](adr/0002-ratio-primitive.md) for why it is an edge logic rather than an extension of the `gain` node.

Both operands are **named parameters**, not positions, and neither is consumed:

| param | role | default |
|---|---|---|
| `control_node` | denominator | required |
| `numerator_node` | numerator | `origin` |
| `threshold` | denominator floor | `GSSK_RATIO_EPSILON` (1e-9) |

The denominator is floored so the flow saturates rather than diverging as the control approaches zero. A model whose control rides the floor is reporting a bounded constant, not a quotient.

`numerator_node` matters because an edge is a flow and therefore **debits its origin**. Without it, the only way to put a stock in a numerator is to drain it — a price mechanism reading the money supply would eat the money supply. Naming the numerator separates *what the flow is computed from* (read, never consumed) from *where the flow goes* (origin → target, still a real flow). Pin the origin with a `source` or `constant` node when that flow must cost nothing; `compute_derivatives` holds those types at `dQ/dt = 0`, so the debit is discarded. See [ADR 0005](adr/0005-price-relaxation-and-named-numerator.md).

This does not weaken the topology rule above — edges are still flows only. State continues to be *read* through named params, never through an edge.

---

## Forcing Functions

Odum draws eleven forcing functions in *Systems Ecology* Fig. 7-2. They are not
eleven mechanisms. They are a **node-value versus edge-rate** distinction
crossed with a **carrier** distinction — and GSSK already models carriers as
Position 1 on nodes and edges. So the eleven collapse to **one waveform
vocabulary attachable in two places**:

| Attach `forcing` to | It drives | Odum's annotation |
|---|---|---|
| a **node** | the node's held value | X / N — a *force* |
| an **edge** | the edge's rate `k` | J — a *flow* |

```json
{ "id": "sun", "type": "source", "value": 1.0,
  "forcing": { "waveform": "sine", "mean": 200, "amplitude": 150,
               "period": 365, "phase": 91.25, "min": 0 } }
```

See [ADR 0006](adr/0006-forcing-one-vocabulary-two-attachments.md) for why this
shape rather than eleven node types, and
`examples/forced_source_model.json` for a worked model.

### The vocabulary

With `tau = t - t_on` and `frac(x) = x - floor(x)`:

| Waveform | Formula | Parameters |
|---|---|---|
| `step` | `v0` if `t < t_on`, else `v1` | `t_on`, `v0`, `v1` |
| `impulse` | `area/w` on `[t_on, t_on+w)`, else `0`, with `w = config.dt` | `t_on`, `area` |
| `ramp` | `v0` if `t < t_on`, else `v0 + slope*tau` | `t_on`, `v0`, `slope` |
| `sawtooth` | `mean + amplitude*(2*frac((tau-phase)/period) - 1)` | `period`, `phase`, `mean`, `amplitude` |
| `square` | `mean+amplitude` while `frac((tau-phase)/period) < duty`, else `mean-amplitude` | `period`, `phase`, `duty`, `mean`, `amplitude` |
| `sine` | `mean + amplitude*sin(2*pi*(tau-phase)/period)` | `period`, `phase`, `mean`, `amplitude` |
| `exponential` | `v0` if `t < t_on`, else `v0*exp(rate*tau)` | `t_on`, `v0`, `rate` |
| `jitter` | `mean + amplitude*(2u-1)`, `u` from the instance RNG | `mean`, `amplitude` |

`t_on` defaults to `config.t_start`. `min` and `max` clamp the result **after**
the formula, so the waveform and its bound stay separately legible; omit either
for no bound in that direction.

### Conventions worth stating plainly

**`phase` is a time offset, not an angle.** It is in the same units as `t` —
not radians, not a fraction of the period — and it is **subtracted**, so a
positive phase *delays* the waveform. In the example above, `period: 365` with
`phase: 91.25` moves the peak from day 91 to midsummer at day 182.5. An
ambiguous phase convention is how two implementations diverge while both look
correct.

**`impulse` is area-normalised.** It delivers `area / dt` over one nominal step,
so its *integral* is `area` at any `dt`. A bare amplitude would make the
delivered quantity depend on step size — a discretisation artefact dressed up as
physics.

**A storage node cannot be forced.** Its value is the *integral* of its flows,
so forcing it asserts two different things about one quantity. `GSSK_Init`
returns `GSSK_ERR_SCHEMA_VIOLATION` naming the node rather than ignoring the
block. Force the source that feeds it, or the edge that drains it.

**A periodic waveform needs a positive `period`.** `sine`, `square` and
`sawtooth` are rejected without one, rather than quietly behaving as constants.

### Evaluation happens at solver stage times

Waveforms are sampled at the RK4 / DOPRI5 **stage** times, not once per step.
This is not a detail. Sampling once per step leaves the forcing first-order
while the state is fourth- or fifth-order — the run completes, the trajectory
looks smooth, and the only way to see it is a convergence study. Integrating a
sine-forced source against its closed form:

| | dt 0.2→0.1 | dt 0.1→0.05 | dt 0.05→0.025 |
|---|---|---|---|
| stage times (what GSSK does) | 16.02× | 16.01× | 16.00× |
| once per step | 1.97× | 1.98× | 1.99× |

### Jitter and reproducibility

`jitter` draws from the instance-owned SplitMix64 stream (`GSSK_SetSeed` /
`GSSK_NextRandom`), never libc `rand()`, and is **latched once per accepted
step**. A fresh draw per stage would make the trajectory depend on solver
internals: the same model would answer differently under `rk4` and `dopri5` for
reasons that are not physics.

`GSSK_Reset` deliberately does **not** rewind the random stream —
`GSSK_EnsembleForecast` and `GSSK_CalibrateMonteCarlo` perturb and then reset
once per run, and rewinding would collapse an ensemble to a single trajectory.
To repeat a jitter run exactly:

```c
GSSK_SetSeed(inst, GSSK_GetSeed(inst));
GSSK_Reset(inst);
```

### Asking the kernel, rather than reimplementing it

```c
int    GSSK_GetNodeForcingKind(GSSK_Instance *inst, size_t node_idx);
int    GSSK_GetEdgeForcingKind(GSSK_Instance *inst, size_t edge_idx);
double GSSK_EvaluateNodeForcing(GSSK_Instance *inst, size_t node_idx, double t);
double GSSK_EvaluateEdgeForcing(GSSK_Instance *inst, size_t edge_idx, double t);
```

These are the **same evaluator the derivative path uses**, exported so a
consumer can render a forcing curve without reimplementing the formulas — a
reimplementation diverges, which is the whole reason they exist. Flat scalars,
not a struct pointer, so no layout crosses the WASM boundary.

`jitter` ignores `t` and returns the value latched for the current step; drawing
fresh would advance the RNG, so merely asking what the model is doing would
change what it does. An unforced element evaluates to its declared `value` or
`k`, so a consumer can plot everything on one axis without first asking which
elements are forced.

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
