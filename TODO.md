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
- [x] `GSSK_SerializeModel(inst, char** out_json)` — emit full topology.
- [x] `GSSK_SerializeSnapshot(inst, char** out_json)` — emit topology + state.
- [x] Snapshot block contents:
  - Current `t`, `dt`, step counter.
  - Full `Q[]` and `Tr[]` vectors keyed by stable node ID, not index.
  - Per-edge `k` (captures cybernetic adjustments).
  - Solver state: confidence, IDC eligibility flags, last divergence event.
  - RNG state (null placeholder — no RNG in core yet).
  - Optional event queue (for adaptive solver mid-step).
- [x] `GSSK_Init(json, ...)` must accept a snapshot-bearing JSON and resume
      from `t = snapshot.t` rather than `t = config.t_start`.
- [x] Property test: `Init → Step×N → Serialize → Init → Step×M` is
      bit-identical to `Init → Step×(N+M)` for the same seed.

---

## Phase 1 — IDC as Baseline (No Silent Fallback)

### 1.1 Riccati Exact Duet (interaction edges)
- [x] Derive duet-of-exponentials closed form for `F = k·Q_origin·Q_control`
      following Giannantoni 2006 §3 / Bastianoni 2011 set-theory exposition.
- [x] Implement `network_is_isolated_duet()` + exact closed-form path in
      `idc_step_ex()`: `Q_A = S·Q_A₀/(Q_A₀ + Q_B₀·exp(k·S·dt))` where S conserved.
- [x] Handle the chained / cyclic case — documented n-et generalisation in
      `docs/giannantoni_assessment.md`: exact duet for n=2, Padé linearisation
      for n≥3 chains/cycles; deferred full n-et algebraic solver.
- [x] Unit tests vs analytical Lotka-Volterra solutions:
      `testRiccatiIsolatedDuetExact` (1e-10 accuracy vs closed form),
      `testRiccatiDuetConservationAcrossRun` (S invariant over 10 steps to 1e-12).
- [x] Numerical comparison vs RK4 via per-step `step_error` estimate.

### 1.2 Padé Treatment for Saturation (limit edges)
- [x] Implement order-(3,3) Padé approximant `N(X)/D(X)` where `X = A·dt`
      as `expm_pade33()` — A-stable, replaces Taylor-6 `expm_vec()`.
- [x] Limit edges included in IDC flow matrix via effective conductance:
      `g = k·C/(C+Q)` — Michaelis-Menten linearisation at operating point.
- [x] Expose per-edge error bound via `GSSK_GetEdgeErrorEstimate(inst, edge_idx)`.
- [x] Step-level max error via `GSSK_GetStepErrorEstimate(inst)`.
- [x] Derive and publish closed-form error bound `|F_padé − F_exact|` as a
      function of `Q/C` in `docs/LIMIT_LOGIC.md`:
      relative error = |ΔQ|/(C+Q₀); per-step bound = k·Q₀·C·dt/(C+Q₀)²;
      max at Q₀=C, zero in linear and saturated limits.

### 1.3 Event Detection (threshold edges)
- [x] Implement bracketing event detector: scan threshold edges for
      sign-change of `Q_origin − θ` between RK4 start and end states.
- [x] Locate root via Illinois algorithm (up to 64 iterations) using
      heap-based `rk4_step_alloc()` to avoid clobbering inst scratchpads.
- [x] Emit `GSSK_EventInternal { t, edge_id, direction }` to growing event log.
- [x] `GSSK_GetEventCount`, `GSSK_GetEventTime`, `GSSK_GetEventEdgeID`,
      `GSSK_GetEventDirection` accessors exposed in C, Swift, TypeScript.
- [x] Handle simultaneous events (multiple thresholds in same interval).
      `do_threshold_substep`: emits all events with tc ≤ best_tc + 1e-10·t_rem
      in each iteration; sequential crossings handled by restarting the loop.
- [x] Handle degenerate cases: tangent crossings, exact-equality starts.
      Guard: skip edge if |Q_origin − threshold| < 1e-12·(1+|threshold|),
      preventing spurious re-detection when origin lands exactly on threshold.
- [x] Restart integrator from event point with logic toggled (sub-stepping).
      `do_threshold_substep` replaces the old event detection loop; advances
      to Q_cross_b after each crossing and continues for the remaining interval,
      up to GSSK_MAX_EVENTS_PER_STEP=8 iterations per GSSK_Step call.

### 1.4 Solver Confidence Reformulation
- [x] `GSSK_ReclassifyNetwork()` always sets `incipient_eligible = true` —
      all edge types now have defined IDC treatment (no silent fallback).
- [x] Per-edge error estimates computed each AUTO/INCIPIENT step.
- [x] `GSSK_SolverConfidence` HIGH/DEGRADED now reflects per-step IDC vs RK4
      disagreement rather than edge-type eligibility.
- [x] RK4 remains an explicit user choice (`method: "rk4"`) for debugging.
- [x] Updated `docs/giannantoni_assessment.md` with Phase 1 implementation notes.

---

## Phase 2 — Adaptive Numerics

### 2.1 Adaptive Time-stepping
- [x] Implement Dormand-Prince 5(4) embedded pair as the *explicit*-method
      reference (used for cross-checking IDC, no longer for production).
      `dopri5_step()`: 7-stage Butcher tableau; reuses inst->dQ,k2-k4; adds k5,k6,k7.
- [x] PI step-size controller with safety factor, min/max step bounds.
      `dopri5_new_h()`: I-controller h_new = h·0.9·(1/err)^0.2, FACMIN=0.2, FACMAX=5.0.
- [x] User-facing tolerances: `rel_tol`, `abs_tol` in config.
      Defaults 1e-6/1e-9; parsed from JSON; used in WRMSE error norm.
- [x] Integrate event detection (1.3) with adaptive stepping — step
      acceptance must not skip over events.
      `adaptive_step_ex()` calls `do_threshold_substep()` on each accepted sub-step.
- [x] Adaptive stepping for IDC: estimate Padé truncation error and shrink
      `dt` until below tolerance.
      DOPRI5 error norm drives step rejection in `adaptive_step_ex()`.
- [x] Maximum-step heuristic from spectral radius of flow matrix.
      `gershgorin_spectral_bound()` → h_max ≈ 3.5/λ; caps config.h_max if zero.

### 2.2 Stability & Conservation Diagnostics
- [x] Per-step total-mass / total-energy conservation check (sum of Q for
      closed sub-systems should be invariant within tolerance).
      `closed_system_conservation_error()`: only fires for fully-closed topologies.
- [x] Warn on conservation violation > tolerance — likely a modelling bug.
      `on_conservation_warning` hook called in `adaptive_step_ex()`; threshold 1e-6.
- [x] Configurable diagnostic hooks: `on_step`, `on_event`,
      `on_conservation_warning`.
      `GSSK_DiagHooks` struct; `GSSK_SetDiagHooks()` API; hooks wired in adaptive path.

---

## Phase 3 — Sensitivity Analysis

### 3.1 Forward Sensitivity
- [x] Augmented-system approach: `dS/dt = J·S + ∂f/∂p` integrated alongside
      state.
      `sens_euler_step`: called at start of GSSK_Step; updates S[n×m] in-place.
      `build_jacobian`: n×n Jacobian ∂(dQ_i/dt)/∂Q_j for all edge logics.
      `compute_param_deriv`: n-vector ∂(dQ_i/dt)/∂k_j for each edge logic.
- [x] `GSSK_EnableForwardSensitivity(inst, param_edge_indices, param_count)`.
      Allocates S[n×m], stores tracked edge indices; reset clears S.
- [x] `GSSK_GetSensitivity(inst, node_idx, param_idx)` returning `∂Q_i/∂k_j`.
      Row-major S[i*m+j]; param_idx is column into the registered param array.
- [x] Hand-coded `∂f/∂p` derivatives per logic primitive (no autodiff).
      CONSTANT:1, LINEAR:Q, INTERACTION:Q·Qctrl, LIMIT:Q·C/(C+Q), THRESHOLD:0/1.

### 3.2 Adjoint Sensitivity
- [x] Backward integration of adjoint system after forward run completes.
      `GSSK_RunAdjoint`: stores full trajectory (n×(steps+1) doubles), then
      integrates dλ/dt = -Jᵀ·λ backward via Euler.
- [x] Cheaper than forward sensitivity when `n_outputs << n_params`.
      Adjoint scales as O(n²·T/dt), independent of number of parameters.
- [x] `GSSK_RunAdjoint(inst, targets, count, param_edges, m, out_gradient)`.
      Objective: L = ½Σ weight_i·(Q_i(T)−target_i)²; terminal λ(T) = ∂L/∂Q(T).
- [x] Checkpoint scheme via full-trajectory malloc (steps × n doubles).
      Memory-bounded alternative (diskIO/chunked) deferred to Phase 4.

### 3.3 Transformity Sensitivity
- [x] Sensitivity of `Tr[i]` to `k[j]` — natural emergy-accounting question.
      `compute_quality_sensitivity`: rebuilds M=(I-F) matrix and solves
      M·(∂Tr/∂k_j) = (∂F/∂k_j)·Tr via Gaussian elimination.
- [x] Derive via implicit-function theorem on the `(−Aᵀ)·Tr = b` system.
      ∂F[tgt][orig]/∂k_j via partition-fraction differentiation; replicate→0.
- [x] `GSSK_GetTransformitySensitivity(inst, node_idx, edge_idx)` → scalar.

### 3.4 Replace Monte-Carlo Calibration with Gradient Descent
- [x] `GSSK_CalibrateGradient`: L-M calibration using forward sensitivity.
      Builds J[n_obs × m] from per-step GetSensitivity, solves (JᵀJ+λI)Δk=−Jᵀr.
      Accepts step if MSE decreases; doubles λ on failure (up to 4 retries).
- [x] `GSSK_CalibrateMonteCarlo` = original DE path (renamed from GSSK_Calibrate).
      `GSSK_Calibrate` retained as backward-compatible shim → Monte-Carlo path.

---

## Phase 4 — Replay & Observability

### 4.1 Mutation Log
- [x] Every successful `GSSK_AddNode` / `GSSK_AddEdge` / `GSSK_DeactivateNode` /
      `GSSK_DeactivateEdge` / `GSSK_SetEdgeK` appends a record to the
      instance's mutation log.
- [x] Record schema: `{t, op, target_id, payload, cause}`.
- [x] `cause` can be `"user"`, `"calibration"`, `"event:<edge_id>"`,
      `"cybernetic:<rule_id>"` — captures *why* the mutation happened.
- [x] `GSSK_GetMutationCount` / `GSSK_GetMutationRecord` provide read-only view.
      `GSSK_SetMutationCause` / `GSSK_ClearMutationLog` for management.
- [x] Mutation log serialises into the snapshot block.
      `GSSK_ExportMutationLog` exports as standalone JSON array.

### 4.2 Replay Engine
- [x] `GSSK_Replay(initial_json, mutations_json, target_t, out_inst)`.
- [x] Rebuilds an instance by applying the original topology, then stepping
      forward and applying each logged mutation at its recorded `t`.
- [x] Determinism: same initial JSON + same mutation log produces identical
      trajectory for fixed-step solvers (floating-point reproducible).

### 4.3 Diff Tooling
- [x] `gssk diff snap_a.json snap_b.json` — per-node state delta table.
- [x] `gssk replay model.json [mutations.json] [--until t=N]` — CLI replay.

### 4.4 Observability Hooks
- [x] `on_mutation` callback added to `GSSK_DiagHooks` — fired on every
      successful topology mutation.
- [x] `on_divergence` callback added to `GSSK_DiagHooks` — fired when NaN/Inf
      is detected before `GSSK_ERR_DIVERGENCE` is returned.
- [x] Swift bindings: `GSSKMutation`, `mutationCount`, `mutation(at:)`,
      `mutations`, `setMutationCause`, `clearMutationLog`, `exportMutationLog`,
      `GSSKSimulator.replay(from:mutations:until:)`.
- [x] 8 new Phase 4 Swift tests (51 at Phase 4 completion).

---

## Phase 5 — Reference Example: Household Ecological-Economy

### 5.1 Multi-Carrier Schema Extension
- [x] Top-level `carriers` array: `[{id, unit, conserved}]`.
- [x] Each edge declares its `carrier` (Odum Position 1 — promoted from
      optional metadata to first-class typed field).
- [x] Conservation enforced *per carrier*, not globally:
      `GSSK_GetCarrierConservationError(inst, carrier_idx)`.
- [x] Cross-carrier coupling expressed via `interaction` edges where the
      control node belongs to a different carrier (bank_account controls
      grocery_receive; news_inflow controls attention_to_decisions).
- [x] Validation fix: LIMIT edges accept `threshold > 0` as saturation
      constant C (not just `control_node`); INTERACTION still requires
      `control_node`.

### 5.2 Household Model Build
- [x] Carriers: money (AUD), energy (kWh), material (kg), information
      (decisions/month).
- [x] Storages:
  - Money: `bank_account`, `credit_card_debt`, `super_fund`.
  - Energy: `grid_credit`, `solar_battery`, `vehicle_fuel`, `body_energy`.
  - Material: `pantry`, `fridge`, `wardrobe`, `appliances_stock`, `waste_bin`.
  - Information: `household_attention`, `pending_decisions`.
- [x] Sources: salary, sunlight, mains_water, grocery_market, news_inflow.
- [x] Sinks: tax, waste_landfill, heat_loss, forgotten_decisions.
- [x] All five logic types present in `examples/household_model.json`:
  - CONSTANT: salary_in, super_contrib, mortgage_out, tax_out, water_fill, attention_replenish.
  - LINEAR: grocery_payment, solar_charge, battery flows, body_metabolism, vehicle_burn, spoilage, decay.
  - INTERACTION: grocery_receive (control=bank_account), attention_to_decisions (control=news_inflow).
  - LIMIT: fridge_eat (threshold=3.0, no control_node).
  - THRESHOLD: credit_payment (threshold=500.0).
- [x] `quality_input` set on salary and sunlight sources for emergy accounting.
- [x] Integrated into `make test` via `MODELS = $(wildcard examples/*.json)`.

### 5.3 Documentation & Tutorial
- [x] `docs/examples/household/README.md` — full walkthrough: node inventory,
      edge logic explanations, emergy accounting, steady-state derivation,
      cross-carrier sensitivity, analytical formula vs simulation output.
- [x] Annotated JSON: `examples/household_model_annotated.json` — `_note`
      fields on every node, edge and config block (kernel ignores unknown keys).
- [x] Notebook: `examples/household_notebook.ipynb` — uses CLI + CSV;
      sections: multi-carrier time series, steady-state verification,
      cross-carrier sensitivity sweep (k_grocery → pantry), summary table.
      Replace subprocess calls with `import gssk` when Phase 6.2 ships.
- [x] Browser demo: `docs/examples/household/demo.html` — self-contained HTML,
      JS RK4 solver, Chart.js charts, sliders for salary/spending/solar/grocery/
      eating-limit/attention parameters, live steady-state display.
      Wired into VitePress sidebar as "Examples → Interactive Demo".

### 5.4 Validation
- [x] `examples/household_model.json` passes `make test` (regression against
      generated expected CSV).
- [x] 10 Phase 5 Swift tests (61 total, all passing):
      carrier count/definitions, node/edge carrier labels, conserved vs
      non-conserved error, pre-step zero, out-of-range index, legacy model
      zero count, household-file all-five-logic-types integration test.
- [x] Steady-state analysis: Q_bank* = (salary − mortgage − tax) / k_spend
      = 26 000 AUD; verified in README and notebook (< 0.1% error vs simulation).
- [x] Cross-carrier sensitivity: doubling k_grocery doubles Q_pantry* (2× ratio
      confirmed in notebook sensitivity sweep and README derivation).

---

## Phase 6 — Productionisation

### 6.1 CI/CD
- [x] Build matrix: gcc/clang × Linux/macOS (`ci.yml` matrix job).
- [x] Swift package build on macOS runner.
- [x] AddressSanitizer + UndefinedBehaviorSanitizer run on full test suite (`make test-asan`, `ci.yml` sanitizers job).
- [x] Valgrind run on regression suite (`make test-valgrind`, `ci.yml` valgrind job).
- [x] Coverage gate: ≥ 85% line via lcov (`make coverage-check`, `ci.yml` coverage job).
- [x] Fuzz target: `tests/fuzz_gssk.c` — `LLVMFuzzerTestOneInput` for `GSSK_Init`. 30 s CI run via `make fuzz-run`.
- [x] Performance regression CI: `make bench-check` gate (500 ms default, configurable via `BENCH_BASELINE_MS`).

### 6.2 Language Bindings
- [x] Python: `python/gssk.py` — full ctypes binding; `make test-python` runs 31 tests.
- [x] JS/TS: `js/gssk.js` — ES module with async `GSSKSimulator.create()`, carrier access, mutation log, TypeScript-compatible.
- [x] Swift: multi-carrier Phase 5 bindings complete; 61 tests passing.

### 6.3 Documentation
- [x] VitePress restructured: Guides (Concepts, Cookbook, Changelog), Reference (API Reference, Schema, Spec, …), Examples.
- [x] `docs/concepts.md` — Odum ESL, node types, edge logic, integration methods, carriers, sensitivity, mutation log.
- [x] `docs/api-reference.md` — C, Python, JavaScript, Swift API tables.
- [x] `docs/cookbook.md` — practical recipes: parametric sweep, sensitivity, snapshot round-trip, multi-carrier JS, mutation log.
- [x] `docs/CHANGELOG.md` — Keep-a-Changelog format, all phases documented.

### 6.4 Benchmarks
- [x] `bench/run_bench.sh` — timing table for all example models + generated bench models.
- [x] `bench/gen_bench_models.py` — generates 10/100/500-node chain benchmarks via `make bench-gen`.
- [x] `make bench` — runs full benchmark table; `make bench-check` — regression gate.

### 6.5 Release & Versioning
- [x] `scripts/release.sh` — bumps `GSK_VERSION_MAJOR/MINOR/PATCH` in `include/gssk.h`, updates CHANGELOG, commits and tags.
- [x] `docs/CHANGELOG.md` — all phases documented; Unreleased section scaffolded.

### 6.6 Security & Robustness
- [x] `SECURITY.md` — vulnerability reporting process, security scope, CI testing instructions.
- [x] `LICENSE` — MIT, with cJSON third-party attribution.
- [x] Fuzz target (`tests/fuzz_gssk.c`) + seed corpus (`tests/fuzz_corpus/`) cover JSON parser attack surface.

---

## Phase 7 — ESL Node Type Taxonomy (Schema v4)

> **Motivation:** GSSK currently encodes Odum's Interaction, Constant Gain,
> Loop-Limited, Exchange, and Switch as `edge.logic` parameters. Per Odum's
> diagramming language these are *nodes* — processing units with internal state
> — not properties of flows. The only true edges in ESL are the flows themselves
> (energy, matter, money, information, code). This phase corrects the topology.

### 7.1 Correct Node/Edge Taxonomy Decision

- [x] **Schema version strategy:** chose **(B) compatibility shim** — `edge.logic`
      kept as v3 alias; v4 models use `type:` node fields; kernel accepts both.
      v3 edge loop skips processing-node edges; per-type helpers compute flow.
- [x] **Exchange node price state:** chose **(A) fixed parameter** — `price` is a
      static node param. Stateful market price deferred to a future phase.
- [x] **Composite macro-expansion timing:** chose **(A) parse-time expansion** —
      composites expand in `GSSK_Init`; mutation log references primitives.

### 7.2 New Fundamental Node Types

- [x] `type: "interaction"` — multi-input production/work gate.
      `compute_interaction_node()`: F = node_k × ∏ Q_origin over all inputs.
      IDC Riccati duet path applies when n=2; Padé for n≥3.
- [x] `type: "gain"` — constant gain amplifier.
      `compute_gain_node()`: F = node_k × Q_control; optional energy draw.
- [x] `type: "loop_limited"` — Michaelis-Menten loop-limited converter.
      `compute_loop_limited_node()`: F = node_k × Q_in / (1 + Q_in / node_C).
- [x] `type: "exchange"` — transaction exchange diamond.
      `compute_exchange_node()`: separates money/goods carriers; atomic debit/credit.
- [x] `type: "switch"` — digital switching box.
      `compute_switch_node()`: flow = node_k × Q_flow if Q_sensor > node_threshold.
- [x] `GSSK_GetNodeTypeString()` added to public API and WASM exports.
- [x] `examples/interaction_model.json` — schema v4 demo with interaction + loop_limited.

### 7.3 Schema v4 & Migration

- [x] Schema v4 accepted by kernel (`schema_version: 4` in metadata block).
- [x] `gssk.h` version bumped to 4.0.0.
- [x] Regression expected CSVs updated (`make test-update`).
- [x] Migration CLI: `./bin/gssk migrate --from 3 input.json` — converts
      `logic: "interaction"` edges to proper `type: "interaction"` nodes;
      `logic: "limit"` → `type: "loop_limited"`; threshold edges skipped
      (semantic incompatibility: v3 constant flow, v4 proportional flow).
- [x] Update existing `examples/*.json` models from v3 to v4 node types
      (oscillator, economic, atwood, saturation migrated; simple/decay bumped).
- [x] Update household model: replaced interaction-edge grocery transaction
      with `type: "exchange"` node coupling money ↔ material carrier.

### 7.4 Solver & Sensitivity Updates

- [x] Multi-input routing: `compute_derivatives` dispatches to per-type helpers
      for all 5 processing node types; handles arbitrary input count.
- [x] Hand-code `∂f/∂p` Jacobian entries for new node types (interaction,
      gain, loop_limited, switch, exchange — added to `build_jacobian`).
- [x] Add new node types to fuzz target: 4 new v4 seeds in
      `tests/fuzz_corpus/` (interaction, loop_limited, exchange, composite).
- [x] Update Swift, Python, JS bindings: `nodeTypeString`, `archetypeCount`,
      `archetypeName`, `compositeCount`, `compositeID`; `GSSKSchemaVersion` → 4.
- [x] Tests: `test_interaction_node()` and `test_loop_limited_node()` in
      `tests/test_advanced.c` (plus 3 Phase 8 archetype tests).

---

## Phase 8 — Composite Node Types & Archetype System

> **Motivation:** Odum's Fig 1.2b composite symbols (Producer, Consumer,
> System Frame, etc.) are combinations of Phase 7 fundamental nodes. Modellers
> should be able to use `type: "producer"` without manually wiring the
> internals. Custom archetypes allow domain-specific composite definitions
> to be supplied in the model JSON. This is the derived-dimensions layer
> of the taxonomy.

### 8.1 Built-in Composite Types

- [x] `type: "producer"` — storage + autocatalytic interaction + sink output.
      Expands to: `{id}__body` storage, `{id}__gate` interaction
      (self-feedback), `{id}__heat` sink; internal edges (`feed_a`, `feed_b`,
      `prod`, `resp`) wired automatically at `GSSK_Init`.
      `params.k_production` overrides gate `node_k`; `params.k_respiration`
      overrides resp edge `k`.
- [x] `type: "consumer"` — storage + metabolism sink.
      Expands to `{id}__body` storage and `{id}__heat` sink with a single
      `metab` linear edge.  `params.k_metabolism` overrides metab `k`.
- [x] `type: "system_frame"` — namespace/encapsulation boundary.
      Structural-only: stored as a `NODE_CONSTANT` (`dQ/dt = 0`) with no
      expansion.  External edges connect directly.
- [x] `type: "misc_box"` — generic unspecified processing unit.
      Expands to a single `{id}__box` storage.
- [x] Composite expansion documented in `docs/cookbook.md`.

### 8.2 User-Defined Archetypes

- [x] Top-level `"archetypes"` block in the model JSON parsed by
      `parse_user_archetypes()` in `src/gssk.c`.
- [x] Any node with `"type": "<archetype_name>"` expands using that
      definition; the instance's `id` is used as a namespace prefix
      (`{id}__{template_id}`).
- [x] Validation: archetype port names default `default_in` to the first
      port (or first node when no ports) and `default_out` to the last.
      Missing internal-id references surface as schema-violation errors.
- [x] `GSSK_GetArchetypeCount`, `GSSK_GetArchetypeName`,
      `GSSK_GetCompositeCount`, `GSSK_GetCompositeID` accessors added
      to `include/gssk.h` and exposed via WASM exports.
- [x] Archetypes live for the lifetime of the instance; the expanded
      primitives are serialised in `GSSK_SerializeSnapshot` as ordinary
      nodes/edges (caller-visible names are the namespaced ones).

### 8.3 Archetype Cookbook & Docs

- [x] `docs/cookbook.md` — added "Using built-in composite node types"
      and "Defining a custom archetype" recipes.
- [x] New example model `examples/producer_consumer_model.json` and three
      new tests (`test_producer_composite`, `test_consumer_composite`,
      `test_user_archetype`) added to `tests/test_advanced.c`.

---

## Phase 9 — Runtime Pattern Discovery (Generativity)

> **Motivation:** Giannantoni's generativity principle holds that systems
> do not merely transform existing qualities but originate new ones through
> recurring, self-stabilising structural patterns. This phase implements a
> kernel-level mechanism to detect such patterns in the live graph and propose
> them as named archetypes — a novel contribution that operationalises
> Giannantoni's framework computationally.

### 9.1 Structural Pattern Detection

- [x] After each `GSSK_Step`, scan the live graph for recurring subgraph
      motifs (2–3 node connected subgraphs) using canonical form enumeration.
      Adjacency matrix built from active edges; all pairs (O(N²)) and triples
      (O(N³), capped at N=64) enumerated per step.
- [x] Motif canonical form is isomorphism-invariant: type strings sorted,
      adjacency bits determined by picking the lex-smallest permutation.
      Handles same-type nodes correctly (up to 6 permutations for 3-node).
- [x] Motif frequency table (up to 256 entries); motifs appearing ≥ 3 times
      per step for ≥ 10 consecutive steps become archetype candidates.
- [x] `GSSK_GetMotifCount`, `GSSK_GetMotifCanon`, `GSSK_GetMotifOccurrence`,
      `GSSK_GetMotifStableSteps`, `GSSK_IsMotifCandidate`, `GSSK_GetMotifSize`,
      `GSSK_GetMotifComplexity` accessors added to public API.

### 9.2 Archetype Proposal API

- [x] `GSSK_ProposeArchetype(inst, motif_idx, name)` — promotes a detected
      candidate motif to a named archetype in the instance's registry.
      Generates generic node ids (node0…nodeN-1), wires linear edges per
      adjacency bits, sets default_in/default_out ports.
- [x] Proposed archetypes logged with `GSSK_MUT_ARCHETYPE_PROPOSAL` op;
      payload = motif canonical string; replayable via `GSSK_Replay`.
- [x] Python/JS/Swift bindings: `motifCount`, `motifCanon`, `motifOccurrence`,
      `motifStableSteps`, `isMotifCandidate`, `motifSize`, `motifComplexity`,
      `generativityIndex`, `proposeArchetype`.

### 9.3 Generativity Metric

- [x] Scalar generativity index G(t) = new_candidates × mean_complexity / dt.
      Zero when no new candidates emerge; spikes when a new motif stabilises.
- [x] `GSSK_GetGenerativityIndex(inst)` — inspired by Giannantoni 2023 §4.
- [x] Exposed in CLI: `gssk run model.json --report generativity` prints
      motif count, G(t), candidate count, and top-10 motifs table to stderr.
- [ ] Document the metric and its theoretical grounding in
      `docs/giannantoni_assessment.md`.

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

- [x] Should `interaction` edges with non-storage origins (i.e. source ×
      storage) get a simplified IDC path?
      **Decision:** No separate path needed. The IDC flow matrix treats the
      source's fixed Q as a constant multiplier; the edge degenerates to
      linear with time-varying k automatically. Confirmed in practice via
      household model (salary_in × bank_account interaction).
- [x] How to handle a topology mutation that *changes* IDC eligibility
      mid-step?
      **Decision:** Complete the current step under the existing
      classification, then call `GSSK_ReclassifyNetwork`; the next
      `GSSK_Step` picks up the new eligibility. Implemented in Phase 3.
- [x] Snapshot schema: include the full `mutation_log` inline, or split
      into a sidecar file referenced by hash?
      **Decision:** Inline. `GSSK_SerializeSnapshot` embeds the mutation log
      in the snapshot block. `GSSK_ExportMutationLog` is also available as a
      standalone export when sidecar behaviour is preferred by the caller.
- [x] Event-detection tolerance vs adaptive-step tolerance: same setting
      or separate?
      **Decision:** Separate. Adaptive stepping uses `rel_tol`/`abs_tol`
      from the model config (WRMSE norm). Event location uses a hard-coded
      Illinois bracket (64 iterations, 1e-12 guard on exact-equality starts).
      Mixing them would make event precision vary with solver tolerance.
- [x] Carrier conservation: hard-enforced (refuse to step if violated) or
      soft (warn and continue)?
      **Decision:** Soft. `GSSK_GetCarrierConservationError` exposes the
      per-carrier error; stepping is never refused. A future strict-mode flag
      (`config.strict_conservation: true`) is the right extension point if
      needed.

---

*Status: draft v0.1. This document is itself a model — revisit at each
phase boundary.*