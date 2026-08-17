# ADR 0003 — Delivered work (W) as explicit low-pass state, not a lagged quality flow

- **Status**: accepted
- **Date**: 2026-08-18
- **Task**: `c2-delivered-work-signal`
- **Supersedes**: nothing
- **Blocks**: `c3-price-dynamics`, `c4-net-energy-feedback-loop`

## Context

Odum's claim is that price is the ratio of circulating money to real work delivered, `P = M/W`. `c1-ratio-division-primitive` (ADR 0002) supplied the division. What is still missing is `W` itself: real work delivered must be **readable inside the derivative pass**, because that is where the ratio is evaluated.

The obstacle is structural rather than arithmetic. The `ratio` primitive consumes its numerator and merely reads its denominator through `control_node`. Expressing `M/W` with *neither* operand consumed therefore needs at least one of them exposed as a signal — a value a flow can be computed from without drawing substance out of it. `W` is that operand.

The task left two options:

- **(a) Lagged** — reuse the previous step's `GSSK_GetQualityFlow` / net-energy flow, already computed after each step.
- **(b) Explicit state** — a low-pass "delivered work" storage fed by the goods/energy inflow to the structure, giving a synchronous `W`.

## Decision

**Option (b), explicit state.**

Two facts about the existing quality machinery decide it, and both were checked in the source rather than assumed.

**Quality accounting is optional, and W must not be.** `quality_enabled` is set to `any_quality`, which is true only when some node declares `quality_input > 0`. `GSSK_GetQualityFlow` returns `NULL` otherwise. Under (a), a model that does not happen to enable emergy accounting would have no `W` at all, and price formation would silently stop working — not fail, *stop*, with a plausible-looking constant price. That couples two features which have no business being coupled: a modeller reasoning about prices should not have to know that transformity must be switched on somewhere else in the file for prices to move.

**Quality flow is not delivered work.** `quality_flow` is transformity-weighted — it is empower, `Tr × F`, not `F`. Using it as `W` would silently substitute a different quantity, one whose units depend on the transformity convention the model happens to adopt. The error would not be visible in the trajectory; it would appear as a price level that is wrong by a factor nobody can locate.

The lag is the lesser objection, though it is real: `W` feeds a negative feedback loop through price, and a one-step delay in a feedback path is exactly the arrangement that produces oscillation as the loop gain rises. Option (b) avoids having to bound `α` against a delay that need not exist.

## Consequences

**`W` becomes visible state.** A low-pass storage appears in the state vector, in CSV output, and in snapshots. It can be plotted, asserted on in a golden trajectory, and inspected when a price looks wrong — none of which is true of a value recomputed inside the derivative pass. For a kernel whose case rests on reproducibility, a quantity that drives results should be in the record.

**The low-pass is a modelling choice with a time constant.** `dW/dt = F_in − β·W` settles at `W ≈ F_in/β`, so `β` sets both the smoothing window and the scale of `W`. That scale is absorbed into the price coefficient, so `β` and `α` are not independent; whichever model lands first should say so explicitly rather than leaving two knobs that appear separable and are not.

**The open question is how `F_in` reaches `W`.** Existing primitives can read a *stock* without consuming it — `interaction` and `ratio` both do, via `control_node` — but there is no primitive that reads a *flow* without duplicating the draw on its origin. Adding a parallel edge to `W` would double-debit the source. Resolving that is the implementation work, and the likely shape is a node type or edge mode that mirrors an existing flow into an accumulator without participating in it. That choice should be recorded here when made, since it determines whether `W` is exact or itself an approximation.

**Tier 1 stays the anchor.** `examples/emergent_price_model.json` reaches the ratio as the fixed point of a relaxation and remains the regression baseline. `c3-price-dynamics` should add the true `M/W` model beside it, so the difference between approximating the ratio and computing it stays visible in the suite.
