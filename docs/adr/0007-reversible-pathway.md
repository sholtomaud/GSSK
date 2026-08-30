# ADR 0007 — The barb-less pathway as a new `reversible` edge logic, exactly integrable

- **Status**: accepted
- **Date**: 2026-08-30
- **Task**: `gip1-g3-reversible-pathway`
- **Supersedes**: nothing
- **Blocks**: nothing

## Context

Odum draws two distinct pathway kinds, and the distinction lives in the notation:

> Where the flow depends only on the force behind it, an arrowhead (barb) is used… Where the flow depends on the difference between the force at one end and the back force from the other end, a line is used without a barb, and this pathway may flow in either direction.
>
> — *Modeling for All Scales*, p.23

Every GSSK logic computes flow forward from origin quantities. `constant` reads nothing, `linear` reads `Q_origin`, `interaction` and `limit` read the origin and a control, `ratio` reads a numerator and a denominator. **None of them reads the target.** There is no back-force term anywhere in `include/gssk.h` or `src/gssk.c`, so the entire second class of pathway — diffusion, exchange across a gradient, any equilibrating process — is inexpressible.

This is not the `exchange` node. That is the transaction diamond: a money/goods counter-flow between two *carriers*, where two barbed pathways run in opposite directions. A barb-less pathway is **one** pathway whose sign is determined by a gradient. Odum draws them differently because they are different things.

## Decision

**A new `reversible` edge logic, appended to `GSSK_LogicType`.**

```
F = k · (Force_origin − Force_target)
```

with **force defaulting to the node quantity**, so `F = k · (Q_origin − Q_target)`.

The flow is **signed**, and the integrator's existing application does the rest:

```c
deriv[e->origin_idx] -= flow;
deriv[e->target_idx] += flow;
```

A negative `F` therefore transports backwards along the declared direction with no special case anywhere. `origin` and `target` stop meaning "from" and "to" for this logic and start meaning only "the first and second end", which is exactly what a barb-less line means in the diagram.

### Why not two opposed `linear` edges

This is the obvious workaround and it is wrong in two ways.

`A→B` with `k₁` plus `B→A` with `k₂` gives a net `k₁·Q_A − k₂·Q_B`. That equals the gradient form **only when `k₁ == k₂`**, and nothing in the model, the schema or the kernel enforces that equality. A model whose two conductances have drifted apart is no longer describing a gradient — it is describing two independent barbed pathways that happen to point at each other, and it will equilibrate to `Q_A/Q_B = k₂/k₁` rather than to equality, silently.

More fundamentally: the pair **is** two barbed pathways in the diagram. Odum's notation uses two opposed arrows for a genuinely different construct (the counter-current, which GSSK already has as `exchange`). Encoding a barb-less line as two barbed ones puts the implementation at odds with the diagram vocabulary the kernel exists to execute — the same objection ADR 0002 raised against overloading `gain` to perform division.

### Why not a negative `k` on `linear`

`linear` with `k < 0` gives `F = k·Q_origin`, which still never reads the target. It produces a pathway that pumps *into* its origin at a rate set by its origin, which is not a gradient and not anything Odum draws.

### Force is the node quantity, for now

Odum's "force" is `X` or `N`, and for a storage it is the stored quantity. Introducing a separate force-per-node concept would be a second primitive wearing this one's clothes. Defaulting force to `Q` keeps `reversible` one primitive; if a force that differs from the quantity is ever needed, it attaches here as a node property without changing this logic's shape or its ABI slot.

## Consequences

### It is exactly integrable, unlike `limit` and `ratio`

`k·(Q_origin − Q_target)` is **linear in the state**, so `build_flow_matrix` gets exact entries rather than a linearisation about the current operating point:

```c
A[tgt][orig] += k;   A[orig][orig] -= k;
A[tgt][tgt]  -= k;   A[orig][tgt]  += k;
```

`reversible` is therefore fully **incipient-eligible**: the IDC matrix exponential integrates it exactly, and IDC and RK4 agree to solver tolerance rather than to a linearisation error. This is a real difference from `limit` (linearised Michaelis-Menten) and `ratio` (exact only in the numerator, for a frozen denominator), and it is why `reversible` is *not* added to the list of logics that force an RK4 fallback.

It is also the first logic whose flow matrix touches **four** entries rather than two. Anything that assumes a one-column contribution per edge — the Jacobian's `dvar`/`ctrl` pair, in particular — needs a second variable index, not a second control node.

### A backward flow carries no transformity

The quality pass already clamps `flow ≤ 0` out of the transformity system (`if (!e->active || flow[ei] <= 0.0) continue;`). With `reversible` that clamp stops being incidental and becomes load-bearing, so it is recorded as a decision rather than left as an accident:

**A reversible pathway flowing backwards contributes nothing to transformity propagation.** Only its forward direction carries emergy into the target.

This is the conservative reading. Emergy along a pathway that reverses is a genuinely open question in Odum's accounting — transformity is a memory of the energy required to produce something, and a flow running back up the gradient is not producing the node it arrives at. Attributing emergy to it would invent an answer the source material does not give. The clamp means a reversing edge's contribution simply switches off while it runs backward and resumes when the gradient does, which is defensible, testable, and does not silently corrupt a transformity solve.

Consumers reading `GSSK_GetEdgeQualityFlow` on a reversible edge should expect `0.0` during backward phases. `GSSK_GetFlows` (GIP-0001 G4) reports the signed rate and is the right call for the physics.

### Every logic switch must be updated

A new logic type is not a single-site change — ADR 0002's table applies unchanged, and all eight sites are still in the same functions:

| Site | Function | Consequence if omitted |
| --- | --- | --- |
| `compute_derivatives` | RK4 derivative | flow is zero |
| `build_flow_matrix` | IDC/incipient linearisation | RK4 and IDC disagree silently |
| `compute_edge_flow` | generic flow accessor | reported flows wrong |
| `compute_quality_pass` | transformity propagation | emergy accounting wrong |
| `build_jacobian` | Jacobian | stiff/implicit paths mis-step |
| `compute_param_deriv` | forward sensitivity | ∂F/∂k wrong |
| `compute_quality_sensitivity` ×2 | quality sensitivity | transformity sensitivity wrong |

Plus `parse_logic_type`, `logic_type_str`, the `EdgeLogic` enum in `gssk.schema.json`, and `src/gssk.d.ts`.

### ABI: append, never insert

`GSSK_LOGIC_REVERSIBLE` goes at the **end** of `GSSK_LogicType`. Inserting it would renumber every existing constant for anything already compiled against the header, and the value crosses the WASM boundary as a bare integer where no recompilation happens at all.

### The non-negativity clamp can hide a bad `k`

`GSSK_Step` clamps a negative state to zero after every step. A reversible edge with `k·dt` large enough to overshoot equilibrium will oscillate, and the clamp will absorb the overshoot rather than reporting it — so a too-stiff gradient looks like convergence rather than like the instability it is. This is not new (`linear` has the same exposure) but `reversible` makes it easier to hit, because the equilibrium sits *between* the two nodes rather than at zero. The convergence test asserts neither node goes negative, which catches the clamp doing work it should not have to.

### Conservation is exact, and is the test that matters

`F` is subtracted from one end and added to the other in the same statement, so the pair's total is conserved by construction under any solver. That makes total quantity across a reversible pair the sharpest available regression: it holds to solver tolerance under both `euler` and `rk4`, and any sign or index error in the four flow-matrix entries breaks it immediately.
