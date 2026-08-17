# ADR 0002 — Division as a new `ratio` edge logic, not an extension of the gain node

- **Status**: accepted
- **Date**: 2026-08-18
- **Task**: `c1-ratio-division-primitive`
- **Supersedes**: nothing
- **Blocks**: `c3-price-dynamics`

## Context

The logic set (`constant`, `linear`, `interaction`, `limit`, `threshold`) has no division, so Odum's claim that price is the ratio of circulating money to delivered work — `P = M/W` — cannot be written down. `c-tier1-emergent-price-anchor` reached that ratio only indirectly, as the equilibrium of a relaxation dynamic `dP/dt = k_d·M − k_s·P·W`, whose fixed point `P* = (k_d·M)/(k_s·W)` is *proportional* to `M/W`. That is a usable anchor but it is not the ratio itself: it lags, it needs two hand-tuned coefficients, and its constant of proportionality is an artefact of those coefficients rather than of the system.

The task left the mechanism as an open decision:

- **(a)** a new edge logic `ratio`, `F = k · Q_origin / (Q_control + ε)`
- **(b)** an extension of the existing `gain` node

## What `compute_gain_node` actually does

The decision turns on the gain node's real semantics, so they are recorded here rather than assumed.

`compute_gain_node` (`src/gssk.c`) identifies its operands **positionally, by edge insertion order**:

- the **first** incoming edge supplies the control, and is *not* consumed;
- the **second** incoming edge, if present, is the energy source, and is debited only when it is a `storage` node;
- the flow is `F = node_k × Q_control`.

Two properties matter. First, it is **unary**: one control determines the flow, with the second leg supplying substance rather than a second operand. Second, its leg assignment is **positional** — reordering the edge array silently changes which node is the control.

## Decision

**Option (a): a new `ratio` edge logic.**

The justification is the gain node's positional, unary structure. A ratio is irreducibly **binary** — it needs a numerator and a denominator, both named and distinguishable. Extending `gain` would mean giving its second incoming edge a second, incompatible meaning: today "energy source to debit", under (b) also "denominator", disambiguated by a mode flag. Those two readings collide directly, because a denominator must *not* be consumed while an energy source must be. Selecting between them by flag would make the same topology mean different things depending on a parameter, which is precisely the ambiguity the ESL topology rule exists to prevent.

Edge logic already has what a binary operator needs. `interaction` and `limit` both take a **named** `control_node`, resolved by id rather than by position, and the control is modulating rather than consumed — the exact semantics a denominator requires. `ratio` is therefore the same shape as primitives that already exist and are already tested, rather than a new shape bolted onto a node whose contract does not fit it.

A secondary consideration: `gain` is an Odum symbol with an established meaning (constant-gain amplifier). Overloading it to perform division would put the implementation at odds with the diagram vocabulary the kernel exists to execute.

## Consequences

**Denominator guard.** `F = k · Q_origin / (Q_control + ε)` with an ε floor, so the flow saturates as `Q_control → 0` instead of diverging. ε must be documented as part of the primitive's contract, since it sets the maximum expressible ratio, and a model that silently rides the floor is reporting a bounded number as though it were a quotient.

**Every logic switch must be updated.** A new logic type is not a single-site change. It is switched on in eight places across seven functions, and omitting any one produces a primitive that is correct under one solver and wrong under another — the failure mode ADR 0001 already warns about for `price_node`:

| Site | Function | Consequence if omitted |
| --- | --- | --- |
| `compute_derivatives` | RK4 derivative | flow is zero |
| `build_flow_matrix` | IDC/incipient linearisation | RK4 and IDC disagree silently |
| `compute_edge_flow` | generic flow accessor | reported flows wrong |
| `compute_quality_pass` | transformity propagation | emergy accounting wrong |
| `build_jacobian` | Jacobian | stiff/implicit paths mis-step |
| `compute_param_deriv` | forward sensitivity | ∂F/∂k wrong |
| `compute_quality_sensitivity` ×2 | quality sensitivity | transformity sensitivity wrong |

Plus `parse_logic_type` and the `EdgeLogic` enum in `gssk.schema.json`. The incipient-eligibility check should also be reviewed: division is nonlinear, so `ratio` may need the same treatment `limit` receives — linearised about the current operating point rather than treated as exactly integrable.

**The Tier 1 anchor is not invalidated.** `examples/emergent_price_model.json` and its golden trajectory remain the regression anchor. Once `ratio` exists, `c3-price-dynamics` can express `dP/dt = α(M/W − P)` directly; that model should be added alongside the Tier 1 one, not in place of it, so the change from proportional-to-ratio to actual-ratio stays visible in the test suite.
