# ADR 0003 — Delivered work (W) as explicit low-pass state, not a lagged quality flow

- **Status**: accepted
- **Date**: 2026-08-18
- **Task**: `c2-delivered-work-signal`
- **Supersedes**: nothing
- **Blocks**: `c3-price-dynamics`, `c4-net-energy-feedback-loop`

## Context

Odum's claim is that price is the ratio of circulating money to real work delivered, `P = M/W`. `c1-ratio-division-primitive` (ADR 0002) supplied the division. What is still missing is `W` itself: real work delivered must be **readable inside the derivative pass**, because that is where the ratio is evaluated.

The obstacle is not that `W` cannot be *read*. `ratio` already reads its denominator through `control_node` without consuming it, so any existing state node can serve. The obstacle is that **no node represents delivered work in the first place**. Delivered work is a *flow* — goods moving through the transaction — and the state vector holds stocks. Nothing in a model says "this much real work was delivered per unit time", so there is no node for the denominator to point at.

`W` therefore has to be constructed: a stock whose value is driven by a flow and which falls when that flow falls. That is what makes this a design decision rather than a wiring exercise.

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

**How `F_in` reaches `W`: a parallel edge off a pinned origin. No kernel change.**

The concern was that adding a second edge to feed `W` would double-debit whatever supplies the real flow. It does not, provided that origin is *pinned*. `compute_derivatives` ends by forcing `deriv[i] = 0.0` for every source, constant and processing node — they do not accumulate `Q` — so any amount debited from such a node is discarded before integration. A tap edge off a source therefore costs the supply exactly nothing.

`examples/delivered_work_model.json` uses this. The goods flow through the exchange is `F = k × Q_seller × Q_buyer`; the tap reproduces that expression as an `interaction` edge originating at `seller`, a source, with `buyer` as control. `W` then leaks at `β`, giving `dW/dt = F − β·W`.

This is asserted, not assumed. `make test-delivered-work` runs the model with and without the tap and requires the observed trade to be **bit-identical** — inventory and buyer agree to `0.000e+00` across 200 steps. If the pinning rule ever changes, that test fails immediately rather than the error surfacing as a slightly wrong price three tasks downstream.

Two limitations follow, and neither is hypothetical:

- **The tap duplicates the flow *expression*, not the edge.** `k` and the control are written twice, so changing the real path without changing the tap silently desynchronises `W`. Referencing the mirrored edge is what `b2-coupled-edge-live` would enable — `coupled_edge` is parsed into `coupled_idx` today but never read in the derivative pass, so it is declared and inert. Until then the duplication is the cost of avoiding a new primitive.
- **The tap is only free off a pinned origin.** Off a `storage` origin it would double-debit and quietly corrupt the very flow it is measuring. This is a real constraint on where `W` can be attached, not a detail.

**Tier 1 stays the anchor.** `examples/emergent_price_model.json` reaches the ratio as the fixed point of a relaxation and remains the regression baseline. `c3-price-dynamics` should add the true `M/W` model beside it, so the difference between approximating the ratio and computing it stays visible in the suite.
