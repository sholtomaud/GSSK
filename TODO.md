# GSK Production Roadmap

> General Systems Kernel. A domain-specific simulation engine for Odum-style energy systems and General Systems Theory, with first-class support for emergy/transformity accounting, runtime topology mutation,  incipient-calculus integration, and replayable execution.

---

## Vision

A single, embeddable, deterministic numerical engine that is **best in class
for systems-dynamics and ecological-economic modelling specifically** — not a
general-purpose ODE library. The wedge is:

1. JSON schema with emergy/transformity semantics built in.
2. Incipient-calculus solver as baseline, not as fast-path.
3. Topology that can mutate at runtime, with full replay.
4. Deployable from one C99 core to native, WASM, Swift, Python, JS.
5. Honest scholarship in the docs.

## Non-Goals (defend these against feature creep)

- [ ] Maintain explicit non-goals list in `docs/NON_GOALS.md`:
  - No stiff-solver zoo (BDF, Rosenbrock, SDIRK). If a problem is stiff in
    IDC-eligible form, the matrix exponential already handles it; if it is
    stiff *and* not IDC-eligible, that is a modelling problem.
  - No DAE / index-reduction support.
  - No symbolic Jacobian engine.
  - No automatic differentiation framework. Adjoint sensitivity is hand-coded
    per flow primitive.
  - No general scientific-computing parity with SciPy / DifferentialEquations.jl.
  - No GUI builder. The schema *is* the UI contract; visualization is a
    downstream concern.

---

## Phase 0 — Rebrand & Schema Foundation

### 0.1 Rename GSSK → GSK
- [ ] Rename repository, NPM package, Swift product.
- [ ] Bulk-rename C symbols: `GSSK_*` → `GSK_*`, `gssk.h` → `gsk.h`.
- [ ] Preserve a deprecated `gssk.h` shim that `#include`s `gsk.h` and
      emits `#pragma message` for one release cycle.
- [ ] Update WASM `EXPORTED_FUNCTIONS` list and TypeScript definitions.
- [ ] Update all docs, README, schema `$id`.
- [ ] Tag final v2.x release as the last GSSK-named release.

### 0.2 Schema v3
- [x] Introduce top-level `metadata` block: `schema_version`, `created_at`,
      `kernel_version`, `model_hash`.
- [x] Add optional top-level `snapshot` block (see 0.3).
- [x] Add optional top-level `mutation_log` block (see Phase 4).
- [x] Add optional `carriers` array for multi-carrier models (see Phase 5).
- [x] Publish JSON Schema with `$ref` decomposition; validate via the kernel
      and via standard tools (ajv).
- [x] Provide v2 → v3 migration tool: `gsk migrate --from 2 input.json`.
- [x] Keep v2 parsing as a compatibility path for one minor cycle.

### 0.3 Round-trip Serialization
- [ ] `GSK_SerializeModel(inst, char** out_json)` — emit full topology.
- [ ] `GSK_SerializeSnapshot(inst, char** out_json)` — emit topology + state.
- [ ] Snapshot block contents:
  - Current `t`, `dt`, step counter.
  - Full `Q[]` and `Tr[]` vectors keyed by stable node ID, not index.
  - Per-edge `k` (captures cybernetic adjustments).
  - Solver state: confidence, IDC eligibility flags, last divergence event.
  - RNG state (for reproducible ensemble runs).
  - Optional event queue (for adaptive solver mid-step).
- [ ] `GSK_Init(json, ...)` must accept a snapshot-bearing JSON and resume
      from `t = snapshot.t` rather than `t = config.t_start`.
- [ ] Property test: `Init → Step×N → Serialize → Init → Step×M` is
      bit-identical to `Init → Step×(N+M)` for the same seed.

---

## Phase 1 — IDC as Baseline (No Silent Fallback)

### 1.1 Riccati Exact Duet (interaction edges)
- [ ] Derive duet-of-exponentials closed form for `F = k·Q_origin·Q_control`
      following Giannantoni 2006 §3 / Bastianoni 2011 set-theory exposition.
- [ ] Implement `idc_interaction_step()` returning analytically exact result
      where both nodes are storages with known initial conditions.
- [ ] Handle the chained / cyclic case (interaction edges forming a cycle) —
      this is the actual hard problem; document the n-et generalisation.
- [ ] Unit tests vs analytical Lotka-Volterra solutions where they exist.
- [ ] Numerical comparison vs RK4 at very small `dt` to confirm convergence.

### 1.2 Padé Treatment for Saturation (limit edges)
- [ ] Implement order-(3,3) Padé approximant of the Michaelis-Menten
      response inside the IDC matrix construction.
- [ ] Derive and publish closed-form error bound `|F_padé − F_exact|` as a
      function of `Q/C`.
- [ ] Expose error bound via `GSK_GetEdgeErrorEstimate(inst, edge_idx)`.
- [ ] Promote bound to step-level `GSK_GetStepErrorEstimate(inst)`.
- [ ] Document the alternative reformulation (Michaelis-Menten as
      quasi-steady-state of a two-step linear binding process) in
      `docs/LIMIT_LOGIC.md` for users who want exactness over compactness.

### 1.3 Event Detection (threshold edges)
- [ ] Implement bracketing event detector: after each tentative step, scan
      all `threshold` edges for sign-change of `Q_origin − θ`.
- [ ] Locate root via Illinois / Brent's method to user-configurable
      tolerance.
- [ ] Restart integrator from event point with logic toggled.
- [ ] Emit `GSK_Event { t, edge_id, direction }` to event log.
- [ ] Handle simultaneous events (multiple thresholds in same interval).
- [ ] Handle degenerate cases: tangent crossings, exact-equality starts.

### 1.4 Solver Confidence Reformulation
- [ ] Remove network-wide `incipient_eligible` flag.
- [ ] Replace with per-edge confidence: every edge has a defined IDC
      treatment (exact, Padé+bound, or event-bracketed).
- [ ] `GSK_SolverConfidence` becomes per-step error estimate, not a binary.
- [ ] Deprecate RK4 silent fallback path entirely. RK4 remains available as
      an *explicit* user choice for debugging / comparison.
- [ ] Update `docs/giannantoni_assessment.md` to reflect what is now
      actually implemented vs deferred.

---

## Phase 2 — Adaptive Numerics

### 2.1 Adaptive Time-stepping
- [ ] Implement Dormand-Prince 5(4) embedded pair as the *explicit*-method
      reference (used for cross-checking IDC, no longer for production).
- [ ] PI step-size controller with safety factor, min/max step bounds.
- [ ] User-facing tolerances: `rel_tol`, `abs_tol` in config.
- [ ] Integrate event detection (1.3) with adaptive stepping — step
      acceptance must not skip over events.
- [ ] Adaptive stepping for IDC: estimate Padé truncation error and shrink
      `dt` until below tolerance.
- [ ] Maximum-step heuristic from spectral radius of flow matrix.

### 2.2 Stability & Conservation Diagnostics
- [ ] Per-step total-mass / total-energy conservation check (sum of Q for
      closed sub-systems should be invariant within tolerance).
- [ ] Warn on conservation violation > tolerance — likely a modelling bug.
- [ ] Configurable diagnostic hooks: `on_step`, `on_event`,
      `on_conservation_warning`.

---

## Phase 3 — Sensitivity Analysis

### 3.1 Forward Sensitivity
- [ ] Augmented-system approach: `dS/dt = J·S + ∂f/∂p` integrated alongside
      state.
- [ ] `GSK_EnableForwardSensitivity(inst, param_ids[])`.
- [ ] `GSK_GetSensitivity(inst, node_idx, param_idx)` returning `∂Q_i/∂k_j`.
- [ ] Hand-coded `∂f/∂p` derivatives per logic primitive (no autodiff).

### 3.2 Adjoint Sensitivity
- [ ] Backward integration of adjoint system after forward run completes.
- [ ] Cheaper than forward sensitivity when `n_outputs << n_params`, which
      is the typical case for calibration.
- [ ] `GSK_AdjointSensitivity(inst, output_fn, params[]) → gradient`.
- [ ] Checkpoint scheme for memory-bounded adjoint over long runs.

### 3.3 Transformity Sensitivity
- [ ] Sensitivity of `Tr[i]` to `k[j]` — natural emergy-accounting question.
- [ ] Derive via implicit-function theorem on the `(−Aᵀ)·Tr = b` system.
- [ ] Worked example in docs: "which edge most influences final transformity
      of the household consumption node?"

### 3.4 Replace Monte-Carlo Calibration with Gradient Descent
- [ ] Existing `GSK_Calibrate` uses ensemble perturbation; replace with
      adjoint-driven L-BFGS or Levenberg-Marquardt.
- [ ] Keep ensemble path available as `GSK_CalibrateMonteCarlo` for
      comparison and non-smooth objectives.

---

## Phase 4 — Replay & Observability

### 4.1 Mutation Log
- [ ] Every successful `GSK_AddNode` / `GSK_AddEdge` / `GSK_DeactivateNode` /
      `GSK_DeactivateEdge` / `GSK_SetEdgeK` appends a record to the
      instance's mutation log.
- [ ] Record schema: `{t, op, payload, caller_tag, cause}`.
- [ ] `cause` can be `"user"`, `"calibration"`, `"event:<edge_id>"`,
      `"cybernetic:<rule_id>"` — captures *why* the mutation happened.
- [ ] `GSK_GetMutationLog(inst)` returns read-only view.
- [ ] Mutation log serialises into the snapshot block.

### 4.2 Replay Engine
- [ ] `GSK_Replay(initial_json, mutation_log, target_t) → GSK_Instance`.
- [ ] Rebuilds an instance by applying the original topology, then stepping
      forward and applying each logged mutation at its recorded `t`.
- [ ] Determinism: same initial JSON + same mutation log + same RNG seed
      must produce bit-identical state trajectory.
- [ ] Replay must respect adaptive-step decisions: log step sizes too if
      adaptive solver was used, or accept rounding differences and flag.

### 4.3 Diff Tooling
- [ ] `gsk diff run_a.snapshot run_b.snapshot` — per-node state delta.
- [ ] `gsk diff-log log_a log_b` — show where mutation streams diverge.
- [ ] CLI: `gsk replay model.json mutations.log --until t=50 --emit-snapshot`.
- [ ] Step inspector: `gsk step model.json --break-at t=12.3 --inspect node:account`.

### 4.4 Observability Hooks
- [ ] User-registered callbacks fired on: step completion, event detection,
      mutation, conservation warning, divergence.
- [ ] Trace output format compatible with OpenTelemetry / chrome://tracing
      for visualisation of long simulations.
- [ ] Optional structured-log JSONL stream from CLI.

---

## Phase 5 — Reference Example: Household Ecological-Economy

### 5.1 Multi-Carrier Schema Extension
- [ ] Top-level `carriers` array: `[{id, unit, conserved}]`.
- [ ] Each edge declares its `carrier` (already exists as Position 1 in v2
      — promote from optional metadata to first-class typed).
- [ ] Conservation enforced *per carrier*, not globally.
- [ ] Cross-carrier coupling expressed via `interaction` edges where the
      control node belongs to a different carrier (price links money and
      material, EROI links energy and money, etc.).

### 5.2 Household Model Build
- [ ] Carriers: money (AUD), energy (kWh), material (kg), information
      (decisions/day).
- [ ] Storages:
  - Money: `bank_account`, `credit_card_debt`, `super_fund`.
  - Energy: `grid_credit`, `solar_battery`, `vehicle_fuel`, `body_kcal`.
  - Material: `pantry`, `fridge`, `wardrobe`, `appliances_stock`,
    `waste_bin`.
  - Information: `household_attention`, `pending_decisions`.
- [ ] Sources: salary, sunlight, mains_water, grocery_market, news_inflow.
- [ ] Sinks: tax, waste_landfill, heat_loss, forgotten_decisions.
- [ ] Edges across all five logic types. Real ones include:
  - Salary → bank_account (constant, weekly).
  - Bank → mortgage (constant, monthly).
  - Bank × grocery_market → pantry (interaction; spend rate depends on
    both budget and inventory level).
  - Pantry → body_kcal (limit; you can only eat so fast).
  - Pantry → waste_bin (linear, spoilage).
  - Credit_card_debt > threshold → bank_account drain (threshold; minimum
    payment kicks in only above a balance).
  - Solar_battery → grid_credit (linear when full).
  - Attention → pending_decisions (interaction; quality of decisions
    depends on attention reserve).
- [ ] Quality_input set on monetary source and solar source for emergy
      analysis: what's the transformity of a coffee in this household?

### 5.3 Documentation & Tutorial
- [ ] `docs/examples/household/README.md` walks through model construction
      from a household balance sheet.
- [ ] Annotated JSON with inline comments (machine-stripped before parsing).
- [ ] Notebook companion using the Python binding — load model, run, plot
      money/energy/material trajectories, run sensitivity on grocery_k to
      find the highest-leverage budget intervention.
- [ ] Browser demo on the docs site with sliders for key parameters.

### 5.4 Validation
- [ ] Steady-state analysis: at constant salary and constant consumption
      rate, account balance must trend per `(income − expense)·t`.
- [ ] Conservation check: total money conserved when banking-internal flows
      ignored; total kcal in − kcal out − kcal stored = 0.
- [ ] Cross-carrier sanity: doubling grocery price (k on money→pantry edge)
      must reduce pantry steady-state by the right factor.

---

## Phase 6 — Productionisation

### 6.1 CI/CD
- [ ] Build matrix: gcc/clang × Linux/macOS × x86_64/arm64.
- [ ] WASM build verified on every PR.
- [ ] Swift package build on macOS runner.
- [ ] AddressSanitizer + UndefinedBehaviorSanitizer run on full test suite.
- [ ] Valgrind run on regression suite (must be zero leaks, zero errors).
- [ ] Coverage gate: ≥ 85% line, ≥ 75% branch in `src/gsk.c` and
      `src/advanced.c`.
- [ ] Fuzz target: `LLVMFuzzerTestOneInput` for `GSK_Init` against
      arbitrary JSON. Run in CI for 5 min, in nightly for 1 hour.
- [ ] Performance regression CI: 30-node supply chain benchmark must stay
      within 10% of last release's wall-clock time.

### 6.2 Language Bindings
- [ ] Python: `gsk-py` via CFFI or pybind11. NumPy-friendly state accessor.
- [ ] JS/TS: thin wrapper around WASM with proper TypeScript types, async
      `init()`, and a stream-style `step()`/`iterate()` API.
- [ ] Swift: already exists; extend to expose sensitivity and replay.
- [ ] R binding (optional, low priority): ecological-economics audience.

### 6.3 Documentation
- [ ] VitePress site already exists; restructure with:
  - Tutorial (household example).
  - Concepts (Odum positions, transformity, IDC, conservation).
  - Reference (every API call, every schema field).
  - Cookbook (calibration, sensitivity, replay, multi-carrier).
  - Theory notes (Giannantoni, Brown, Bastianoni summaries).
- [ ] All examples in docs must be CI-tested (extract code blocks, run).
- [ ] Versioned docs: separate site per minor release.

### 6.4 Benchmarks
- [ ] Public benchmark suite in `bench/`: 10/100/1k/10k-node networks,
      mixed logic, with reference timings.
- [ ] Compare against SciPy `solve_ivp` and DifferentialEquations.jl on
      equivalent problems for context (not parity claims).
- [ ] Memory profile: peak RSS per 1k nodes.
- [ ] Publish benchmark results per release on docs site.

### 6.5 Release & Versioning
- [ ] Semantic versioning. ABI stability promise: no symbol changes within
      a minor.
- [ ] `GSK_VERSION_MAJOR/MINOR/PATCH` macros and `GSK_GetVersionString()`.
- [ ] Snapshot files carry `kernel_version`; loading a snapshot from a
      newer minor must warn, from a newer major must error.
- [ ] Changelog generated from conventional commits.

### 6.6 Security & Robustness
- [ ] All JSON input length-bounded; deep-recursion guard in cJSON.
- [ ] All allocations checked; OOM never crashes — returns
      `GSK_ERR_MALLOC_FAILED`.
- [ ] No format-string vulnerabilities (`snprintf` everywhere, audited).
- [ ] Threat model document in `docs/SECURITY.md`: kernel is safe to run on
      untrusted JSON input.
- [ ] License (choose: MIT, Apache-2.0, or LGPL — emergy community is
      academic-leaning; Apache-2.0 is the safer default).

---

## Continuous Concerns

- [ ] Every new logic primitive added requires: forward implementation,
      IDC treatment (exact or Padé-with-bound), forward + adjoint
      sensitivity hand-coding, conservation test, regression model.
- [ ] Every PR updates `docs/CHANGELOG.md` and the numerical regression
      suite where relevant.
- [ ] Quarterly review of the non-goals list. If a non-goal is being
      lobbied for, the question is: does this make GSK better at being
      GSK, or worse at being SciPy?

---

## Open Design Questions (decide before Phase 1 ships)

- [ ] Should `interaction` edges with non-storage origins (i.e. source ×
      storage) get a simplified IDC path? Probably yes — degenerates to
      linear with time-varying k.
- [ ] How to handle a topology mutation that *changes* IDC eligibility
      mid-step? Current plan: complete the step under old classification,
      reclassify, take next step under new.
- [ ] Snapshot schema: include the full `mutation_log` inline, or split
      into a sidecar file referenced by hash? Inline is simpler; sidecar
      scales better for long-running ensembles.
- [ ] Event-detection tolerance vs adaptive-step tolerance: same setting
      or separate? Separate, probably — event location wants tighter
      precision than trajectory error.
- [ ] Carrier conservation: hard-enforced (refuse to step if violated) or
      soft (warn and continue)? Soft by default with a strict-mode flag.

---

*Status: draft v0.1. This document is itself a model — revisit at each
phase boundary.*