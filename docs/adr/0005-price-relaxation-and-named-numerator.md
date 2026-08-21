# ADR 0005 — Price relaxes toward M/W rather than being reset to it, and the ratio's numerator becomes a named operand

- **Status**: accepted
- **Date**: 2026-08-21
- **Task**: `c3-price-dynamics`
- **Supersedes**: nothing
- **Amends**: ADR 0002 (completes the "both named" requirement it stated)
- **Blocks**: `c4-net-energy-feedback-loop`

## Context

Odum's claim is `P = M/W`. ADR 0002 supplied the division, ADR 0003 supplied `W`, and `c0-price-constant-or-node-ref` supplied the hook that lets an exchange read price from a state node. What remained was the dynamics: how `P` gets to `M/W`.

The task left one decision open — **relaxation** `dP/dt = α(M/W − P)` versus **algebraic** `P = M/W` recomputed each step — and the phase document recommended relaxation. Implementing it surfaced a second decision that was not anticipated, and it has to be recorded because ADR 0002 asserted the opposite.

## Decision 1 — relaxation, not algebraic assignment

**`P` is a storage node relaxing toward the ratio, with `α` a settable rate.**

Three reasons, in order of weight.

**Algebraic assignment has no home in this kernel.** The state vector is advanced by `compute_derivatives` and an integrator; there is no per-step "recompute these values from those values" pass, and adding one would introduce a second kind of node whose value is not the integral of a flow. That is a structural change to what a GSSK model *is*, made for one quantity. Odum's own diagrams do not contain assignment either — a tank fills and drains, which is exactly a first-order lag.

**Price adjustment takes time, and the time is a modelling claim.** An algebraic price re-clears instantaneously at every RK4 stage no matter how violently `M` or `W` move. `α` makes the adjustment speed an explicit, calibratable parameter of the model rather than an artefact of `dt`. It is the difference between asserting "the market clears infinitely fast" and being able to state how fast.

**`c4` needs the lag to be real.** The next task closes a feedback loop: falling net energy lowers `W`, which raises `P`, which feeds back through the transaction. A loop containing an instantaneous algebraic element has no phase margin to reason about, and its stability then depends entirely on `dt`. With relaxation, the loop's dynamics are set by `α` and `β` — quantities the modeller chose — and `dt` remains what it should be, a numerical parameter that the answer does not depend on.

**The cost, stated plainly:** `P` no longer *equals* `M/W`; it converges to it, and lags whenever `M/W` moves. `tests/test_price_dynamics.c` pins the fixed point to `M/W` to 1e-9 and the approach to `(M/W)(1 − e^{−αt})`, so the lag is a specified property rather than an unmeasured one.

### Authoring form: two edges, one α

A relaxation is two flows, because a flow has two ends and `dP/dt = α(M/W − P)` has two terms. `examples/price_dynamics_model.json` writes them as an inflow carrying `α·M/W` and a drain carrying `α·P`:

```
p_target: unity → price   ratio  { k: α, numerator_node: M, control_node: W }
p_relax:  price → clearing linear { k: α }
```

This is what a tank with an inflow and a drain looks like in ESL, so the authoring form matches the diagram. It has one sharp edge: **`α` is written twice, and the two must be equal.** If they diverge to `k_in` and `k_out`, the fixed point becomes `(k_in/k_out)·(M/W)` — quietly reintroducing the merely-*proportional* behaviour that Tier 1 had and that Tier 2 exists to replace. Nothing in the kernel can catch that, because both models are legal. What catches it is the test asserting the fixed point equals `M/W` **exactly**, and that is why that assertion is written as an equality to 1e-9 rather than as a proportionality.

## Decision 2 — the ratio's numerator becomes a named, non-consumed operand

ADR 0002 closed with "Once `ratio` exists, `c3-price-dynamics` can express `dP/dt = α(M/W − P)` directly." That is not true as written, and the reason is worth recording.

**An edge is a flow, so it debits its origin.** `ratio` computes `F = k·Q_origin / D(Q_control)`. The denominator arrives by name through `control_node` and is read without being consumed; the numerator is `Q_origin` and is therefore *drained by the edge that reads it*. Putting `M` in the numerator means the price mechanism eats the money supply: with `α = 0.5`, `M = 1000`, `W = 2`, the price inflow debits 250 AUD per unit time from a stock the price is supposed to be observing.

ADR 0003 had already named this constraint — "the tap is only free off a pinned origin; off a `storage` origin it would double-debit and quietly corrupt the very flow it is measuring" — and treated it as a limitation of the `W` tap. It is in fact the general obstacle, and `M` is the case where it bites, because `M` is the circulating money stock and cannot be pinned.

**Decision: `params.numerator_node`.** When present, the quotient's numerator is that node's `Q`, read by id and not consumed — the identical contract `control_node` already has. When absent, the numerator is `Q_origin` and every existing model is bit-for-bit unchanged (asserted directly in `test_price_dynamics.c`, not merely assumed from `test_ratio.c` still passing).

This is not a new mechanism so much as the completion of the one ADR 0002 chose. That ADR rejected extending the `gain` node on the grounds that "a ratio is irreducibly binary — it needs a numerator and a denominator, **both named and distinguishable**", and then named only the denominator. `numerator_node` supplies the other name.

### What was considered instead, and why not

- **A non-consuming "observe" flag on any edge.** More general, and it would have given the kernel Odum's information/control line as a first-class thing. Rejected because `docs/concepts.md` states the topology rule as "**edges are flows only**" — state is read through named params, never through an edge. A flag that makes some edges not-flows changes the meaning of the topology, which is exactly what ADR 0002 refused to do when it declined to disambiguate `gain`'s second leg by flag.
- **A `ratio` processing node (v4).** Operands would be identified positionally, by edge insertion order, which is the property ADR 0002 rejected `gain` for.
- **Low-passing `M` into a signal storage and dividing that.** Adds a third time constant that is not a modelling claim about anything, only an artefact of the workaround.

### Where the flow still goes

`numerator_node` changes what the flow *is*, not where it goes: `p_target` still debits `unity` and credits `price`. `unity` is a `source`, and `compute_derivatives` pins sources to `dQ/dt = 0`, so the debit is discarded — the same free-tap mechanism ADR 0003 established. `constant` logic is the existing precedent for a flow whose rate does not depend on `Q_origin`; this is the second.

## Consequences

**Every site that differentiates a ratio must differentiate with respect to the numerator.** `numerator_node` needs no new entry in `parse_logic_type` or the `EdgeLogic` enum, but it does reach the same seven flow paths ADR 0002 tabulated, plus the Jacobian's *column* index. That last one is the dangerous one: `build_jacobian` and `build_flow_matrix` previously assumed the differentiation variable was `origin_idx`. Where they diverge, plain RK4 is unaffected and only the IDC, implicit and forward-sensitivity paths are wrong — silently. `test_price_dynamics.c` runs the same relaxation under RK4 and IDC against the analytic solution for that reason.

**Two pre-existing defects were found and fixed here**, both in serialization, both of which made a `ratio` edge un-round-trippable — which matters directly to the Phase G archival story, where the serialized form *is* the artefact:

- `logic_type_str` had no `ratio` case and fell through to its `"linear"` default. Any model containing a `ratio` edge serialized as a model containing a `linear` edge: the division was silently replaced by a proportional flow, permanently. ADR 0002's eight-site table did not list the serializer, and that is how it was missed.
- `threshold` was emitted only for `threshold` logic, but `ratio` reads it as the denominator floor. A deliberate floor of 0.01 round-tripped back to `GSSK_RATIO_EPSILON` (1e-9) — a 1e7× change in the saturated price.

**Authoring mistakes are rejected rather than reinterpreted.** An unknown `numerator_node` id is a linkage error, and `numerator_node` on non-`ratio` logic is a logic error. Ignoring the latter would leave a model that reads as though the quotient were wired up while the kernel quietly used `Q_origin` — the same class of silent-fallback failure ADR 0004 removed from node types.

**Tier 1 remains the anchor.** `examples/emergent_price_model.json` is unchanged and still in the regression suite. `examples/price_dynamics_model.json` sits beside it, so the difference between a price *proportional to* `M/W` and a price *converging to* `M/W` stays visible in the suite rather than being replaced by it.

**`α` and `β` are still not independent.** ADR 0003 noted that `β` sets the scale of `W` and therefore the level of `P`. That is unchanged: `α` sets how fast price adjusts, `β` sets what `W` means. The example says so at the edge that carries `β`.
