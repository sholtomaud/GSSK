# ADR 0008 — The n-ary work gate as `control_nodes`, and the subtracting action as a new `subtract` logic

- **Status**: accepted
- **Date**: 2026-08-31
- **Task**: `gip1-g1-interaction-sub-and-nary`
- **Supersedes**: nothing
- **Blocks**: nothing

## Context

Odum draws one interaction glyph and lets it compute several different ways. *Modeling for All Scales* Fig. 2.6 shows the same pointed block as (a) a product of two inputs, (c) a product of **three** input forces, (d) a divisor action, and (e) a **subtracting** action. The overloading is deliberate: one shape keeps the diagram legible while the arithmetic inside it varies.

GSSK ships three of the five. `interaction` is (a). `ratio` is (d), and ADR 0002 gave it better semantics than the GIP proposed — a floored denominator so the flow saturates rather than diverging, plus `numerator_node` (ADR 0005) so a stock can enter a quotient without being drained. What is missing is (c) and (e):

- **(c) n-ary product.** `params.control_node` is a single string in `gssk.schema.json`. A work gate with three inputs — `F = k · Q_origin · Q_c1 · Q_c2` — cannot be written at all. The workaround is to introduce an intermediate storage node holding the partial product, which is a *different model*: the intermediate integrates, so it lags, and it appears in the state vector, the emergy accounting and every CSV column list.
- **(e) subtracting action.** No equivalent exists. `reversible` (ADR 0007) computes a difference, but of the origin and the **target** — it is the barb-less pathway, and it is signed by design. The subtracting action is a barbed pathway whose *rate* is set by a difference between the origin and a **control** that it does not consume. Those are not the same statement and neither substitutes for the other.

GIP-0001 G1 proposed a single `params.op` string on `interaction`, taking `"mul"`, `"div"` and `"sub"`.

## Decision

Two separate mechanisms, because (c) and (e) are two separate gaps.

### 1. The n-ary product is an arity change, not a new logic: `params.control_nodes`

```json
{ "logic": "interaction",
  "params": { "k": 0.01, "control_nodes": ["labour", "fuel"] } }
```

`F = k · Q_origin · Π Q_control`. The singular `params.control_node` remains, means exactly what it always meant, and is the one-control spelling; `control_nodes` is the n-ary spelling. **The two are mutually exclusive** — an edge carrying both is rejected with `GSSK_ERR_SCHEMA_VIOLATION` rather than one silently winning, which is the failure mode `limit`'s `control_node`-beats-`threshold` precedence already demonstrates the cost of.

**More than one control node is accepted only by `interaction`.** `ratio` and `subtract` are binary operations — a quotient of three things and a difference of three things are not defined here, and guessing at left-associativity would be inventing semantics Odum's figure does not show. `limit` takes one half-saturation constant. All three reject a second control node at load with `GSSK_ERR_SCHEMA_VIOLATION`. `control_nodes` with exactly one entry is legal everywhere `control_node` is, and identical to it.

The cap is `GSSK_MAX_CONTROL_NODES` (8), a fixed limit in the same house style as `GSSK_MAX_ARCH_NODES`. It is not a considered upper bound on modelling — it is the point past which a product of stores is dominated by floating-point range rather than by the model, and a diagram with nine inputs into one glyph has stopped being legible, which was the reason for the overloaded glyph in the first place.

### 2. The subtracting action is a new appended `GSSK_LogicType`: `subtract`

```json
{ "logic": "subtract",
  "params": { "k": 0.5, "control_node": "demand" } }
```

```
F = max(0, k · (Q_origin − Q_control))
```

**Not `params.op`.** The divisor action already shipped as its own logic type (`ratio`, ADR 0002), so an `op` field would mean `logic: "ratio"` and `logic: "interaction", op: "div"` are two spellings of one thing — the drift this repository's standards exist to prevent, arriving in the published schema where it cannot be corrected quietly. There is a mechanical argument too: `logic` is the branch key at eight switch sites in `src/gssk.c`, and `-Wall -Wextra -Werror` turns a missed enum case into a build failure. An `op` string would be checked by nothing, and the site that forgot it would return `0.0` and look like a modelling result.

Appended after `GSSK_LOGIC_REVERSIBLE`, value 7. The existing values cross the WASM boundary as bare integers and MUST NOT move.

### The clamp at zero is not an implementation detail

`subtract` is a **barbed** pathway. The barb asserts a direction, so a negative flow on it would contradict the notation — it would drain the target and fill the origin along a line that says it cannot. Signed flow is what `reversible` is for, and its barb-less line is how the diagram says so. `max(0, …)` is therefore the semantics, not a guard: the subtracting action saturates at zero when the control overtakes the origin, exactly as a real subtracting gate stops rather than reverses.

## Solver eligibility

Both forms stay incipient/IDC-eligible; `incipient_eligible` is unconditionally true and the question is what the linearisation is, not whether one exists.

- **n-ary `interaction`** is linearised at the current operating point exactly as the binary case is: `conductance = k · Π Q_control`, taken from the current state. This is the same first-order treatment the two-input work gate has always had, with no additional error class — the product of controls is frozen over the step whether there are one or seven of them.
- **`subtract`** contributes four matrix entries, like `reversible`, because the flow depends on two state variables: `A[tgt][orig] += k`, `A[orig][orig] −= k`, `A[tgt][ctrl] −= k`, `A[orig][ctrl] += k`. Where the difference is positive this is **exact**, not a linearisation, since `k(Q_origin − Q_control)` is linear in the state. Where it is at or below zero the edge contributes nothing at all.

That makes `subtract` **piecewise** exact, and the seam matters: an edge whose difference crosses zero *within* a step is integrated as though it stayed on whichever side it started. The error is the same class `threshold` already has and is bounded by `dt`, not by the magnitude of the flow. A model that parks a subtract edge near its own zero should not read its trajectory as exact — reduce `dt` or run `AUTO`, where the IDC/RK4 cross-check reports the disagreement as `GSSK_CONFIDENCE_DEGRADED` instead of hiding it.

## Consequences

- Existing single-control models are **bit-identical**. `control_idx` still holds the one control; `extra_control_count` is zero and the product loop multiplies nothing. No expression that ran before was rewritten.
- `GSSK_LogicType` values 0–6 are unchanged. `subtract` is 7.
- Serialisation round-trips the n-ary form: `build_topology_json` emits `control_nodes` when an edge has more than one control and `control_node` when it has exactly one, so a model reloads as the model it was.
- Quality accounting needs no special case. `subtract` already returns a non-negative rate, so the ADR 0007 clamp in `compute_quality_pass` never fires on it.
- The archetype edge template still admits neither `control_node` nor `control_nodes`, for the reason it never admitted the singular: a template is written before any instance exists, so it has no model node to name.
