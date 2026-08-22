# ADR 0006 — Forcing functions: one waveform vocabulary, two attachment points

**Status:** accepted
**Date:** 2026-08-22
**Task:** `h8-forcing-function-waveforms`
**Depends on:** `h8a-thread-time-through-ode-core`, `h8b-reject-unknown-model-keys`

## Context

A source node held its declared value for the whole run, and whether that constant meant anything at all depended on the edge reading it: `linear` gives `flow = k × value`, `constant` gives `flow = k` and ignores the value entirely.

So GSSK expressed exactly two of the eleven forcing functions Odum draws in *Systems Ecology* Fig. 7-2 — constant force and constant flow — and had no representation for step, impulse, ramp, sawtooth, square, sine, exponential or jitter. For a kernel whose subject is energy systems, that is a large hole: seasonality, pulses, regime shifts and environmental noise are not exotic cases, they are most of what drives an ecosystem model.

The obvious reading of "eleven forcing functions" is that eleven things need building.

## Decision

**One waveform vocabulary, attachable in two places. Not eleven node types.**

```
forcing on a NODE  → drives that node's held value   (Odum X / N, a force)
forcing on an EDGE → drives that edge's rate k       (Odum J, a flow)
```

Odum's eleven annotations are not eleven mechanisms. They are a **node-value versus edge-rate** distinction crossed with a **carrier** distinction — and GSSK already models carriers, as Position 1 (`carrier`) on both nodes and edges. The carrier dimension is therefore already handled, and what remains is eight waveforms × two attachment points.

### Rejected: eleven node types

Adding `sine_source`, `step_source`, `pulse_source` and so on would have:

- multiplied the node taxonomy by the size of the waveform vocabulary, and again by every future waveform;
- put the waveform in the *type* field, where `reject-unknown-node-types` has to distinguish a typo from a legitimate archetype name — so every new waveform would widen the surface that check has to police;
- given no way to force an **edge rate** at all, since a node type cannot attach to an edge. Odum's J annotations would have been unreachable;
- duplicated the evaluator once per type, which is precisely the divergence the shared evaluator exists to prevent (ADR 0001 makes the same argument for the transaction diamond's shared helpers).

### Rejected: a `forcing` block at the model root, keyed by element id

This was the shape the upstream proposal's example JSON implied (`"forcing": {"sun": {...}}`). It was rejected because it separates the waveform from the thing it drives: a reader of the `nodes` array cannot see that `sun` is forced, and an element deleted from `nodes` leaves an orphaned forcing entry that nothing rejects. Attaching `forcing` to the element keeps the model locally legible and makes the orphan case impossible.

## Consequences

### Storage nodes cannot be forced, and that is an error

A storage node's value is the **integral of its flows**. Forcing it is a contradiction — the model would be asserting two different things about the same quantity. `GSSK_Init` and `GSSK_AddNode` both return `GSSK_ERR_SCHEMA_VIOLATION` naming the node, rather than ignoring the block. Silently ignoring it is exactly the failure mode `h8b-reject-unknown-model-keys` was landed to remove; it would have been perverse to reintroduce it here.

### Evaluation happens at stage times, and only a convergence study proves it

This is the requirement that is easiest to get wrong and hardest to notice. A waveform sampled **once per step** rather than **once per stage** leaves the forcing first-order while the state is fourth- or fifth-order. The run completes. The trajectory looks smooth. Nothing is NaN.

`h8a-thread-time-through-ode-core` landed separately to make the stage times available, so that this task's diff is about forcing rather than about threading `t` through 16 call sites. `tests/test_forcing.c` integrates a sine-forced source against its closed form and asserts fourth-order convergence over three refinements.

Measured, both ways:

| | dt 0.2→0.1 | dt 0.1→0.05 | dt 0.05→0.025 |
|---|---|---|---|
| stage times (correct) | **16.02×** | **16.01×** | **16.00×** |
| once per step | 1.97× | 1.98× | 1.99× |

Every other test in the suite passes in both columns. This is the only check that separates them.

### Jitter is latched once per accepted step

A fresh draw per RK4 stage would make the trajectory depend on solver internals: the same model would give different answers under `rk4` (4 stages) and `dopri5` (7) for reasons that are not physics. Drawing on a *rejected* adaptive sub-step would tie the stream position — and so the whole trajectory — to the error controller's search path.

So the draw happens once, at the top of `GSSK_Step` / `GSSK_StepAdaptive`, from the instance-owned SplitMix64 stream (`GSSK_SetSeed` / `GSSK_NextRandom`) rather than libc `rand()`. The test asserts the *draw sequence* is bit-identical across `rk4`, `incipient` and `adaptive` — not the trajectory, which differs in the last bits because the solvers sum their stage weights differently, which is round-off and not stream position.

### `GSSK_Reset` does not rewind the random stream

Making it do so was tried, and it is wrong. `GSSK_EnsembleForecast` and `GSSK_CalibrateMonteCarlo` both perturb parameters with the instance RNG and then call `GSSK_Reset` once per run; rewinding hands every run the same perturbation and collapses the ensemble to a single trajectory. `test_advanced`'s calibration caught it immediately.

`GSSK_Reset` therefore means "back to `t_start`", not "back to the start of the random stream". To repeat a jitter-forced run exactly:

```c
GSSK_SetSeed(inst, GSSK_GetSeed(inst));
GSSK_Reset(inst);
```

This keeps reproducibility available without taking exploration away. It is documented on the evaluator in `include/gssk.h` and asserted in the round-trip test.

### The forced value is written into the observable state

The derivative path substitutes forcing into a scratch copy, which is enough to integrate correctly — but it left `inst->state` showing the node's *declared* value. A sine-forced source would have appeared as a flat line in `GSSK_GetState` and in the CSV while the storage it drives visibly oscillated. The physics would have been right and the output misleading, which is its own kind of quietly-wrong. Forced node values are now written into the live state after each accepted step and once at init.

This is safe precisely because only nodes pinned to `dQ/dt = 0` can be forced — the storage rejection above is what makes it so.

### The evaluator is exposed, and proven not to be a second implementation

`GSSK_EvaluateNodeForcing` / `GSSK_EvaluateEdgeForcing` are exported and in the WASM export list. Without them consumers reimplement the formulas, and a reimplementation diverges — the same argument ADR 0001 makes for the diamond's shared helpers.

They are flat scalar calls, not a `GSSK_Forcing` struct pointer, because pushing a struct layout across the WASM boundary is the hazard `h9-carrier-field-accessors` was landed to remove.

A test drives a source with each of the eight waveforms and asserts that what the evaluator **reports** equals what the kernel **integrated**, at every step — so the two cannot drift.

### The phase convention is stated, not implied

`phase` is a **time offset in the same units as t**. It is not radians and not a fraction of the period, and it is **subtracted**, so a positive phase *delays* the waveform. An ambiguous phase convention is how two implementations diverge while both look correct, so it is written in the header, the schema, `docs/concepts.md` and the example's `_note`.

Clamping (`min` / `max`) is applied **after** the formula rather than folded into it, so the waveform and its bound stay separately legible.

### The impulse is area-normalised

`impulse` delivers `area / w` over a window of width `w = config.dt`, so its **integral** is `area` at any step size. A bare amplitude would have made the delivered quantity depend on `dt` — a discretisation artefact masquerading as physics. The window is taken from `config.dt` rather than the solver's current step, so an adaptive sub-step cannot change it.

### What this does not do

Driving a source from **observed data** — a tabulated series rather than an analytic waveform — is a different thing and is not attempted here. It is tracked as `h8c-data-driven-forcing`.
