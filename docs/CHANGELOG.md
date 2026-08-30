# Changelog

All notable changes to GSSK are documented here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project adheres to [Semantic Versioning](https://semver.org/).

---

## [Unreleased]

### Added

- **`gssk.schema.json` now ships in the npm package.** `package.json` `"files"` listed `dist/`, `include/` and `README.md`, so the only machine-readable statement of the model vocabulary was not published. Downstream consumers hand-maintained their own copy of the node-type enum instead, with nothing to detect drift against the real one. The schema is now in `"files"`, `make check-version` asserts it stays there, and CI asserts `npm pack` actually puts it in the tarball.

- **`reversible` edge logic — Odum's barb-less pathway.** `F = k × (Q_origin − Q_target)`, signed. Raised as GIP-0001 G3; the decision is [ADR 0007](adr/0007-reversible-pathway.md).

  Odum draws two pathway kinds and the distinction lives in the notation: a barb where the flow depends only on the force behind it, no barb where it depends on the difference between the force at one end and the back force from the other, *"and this pathway may flow in either direction"* (*Modeling for All Scales*, p.23). Every GSSK logic computed forward from origin quantities and **none of them read the target**, so the entire second class — diffusion, exchange across a gradient, any equilibrating process — was inexpressible.

  `reversible` is the only logic that reads both ends, and therefore the only one that can transport backwards along its declared direction. For a barb-less line `origin` and `target` name the two ends rather than a from and a to: **swapping them produces an identical trajectory**, asserted step-by-step rather than at equilibrium.

  This is not the `exchange` node, which is the transaction diamond — two barbed pathways carrying a money/goods counter-flow. A barb-less pathway is one pathway whose sign is a gradient.

  Two properties worth knowing:

  - **It is exactly integrable.** Being linear in the state, the incipient/IDC solver integrates it exactly rather than linearising about the operating point, unlike `limit` and `ratio`. It stays IDC-eligible. It is also the first logic whose flow-matrix contribution touches **four** entries rather than two, which is why `build_jacobian`'s second-variable slot is no longer named for the control node — `reversible` depends on its *target*, which is not a control.
  - **A backward flow carries no transformity.** The quality pass's `flow ≤ 0` clamp stops being incidental and becomes a decision: a flow running back up a gradient is not producing the node it arrives at, so `GSSK_GetEdgeQualityFlow` reads `0.0` while the pathway reverses. `GSSK_GetFlows` reports the signed rate.

  `GSSK_LOGIC_REVERSIBLE` is **appended** at 6; every existing `GSSK_LogicType` value is unchanged and pinned by test, because the value crosses the WASM boundary as a bare integer where nothing recompiles.

- **`examples/diffusion_model.json`** — three tanks in a line, two reversible edges, equilibrating from a step gradient. Both edges are declared *against* the physical gradient on purpose, so the example exercises backward transport rather than merely describing it: a build that clamped the flow at zero produces a visibly different CSV. No existing golden moved.

### Fixed

- **The npm package version was five majors behind the kernel.** `package.json` still said `1.0.0` while `include/gssk.h` said `GSK_VERSION_STRING "5.0.0"`; `scripts/release.sh` had only ever bumped the header. That is not cosmetic — the package ships `include/`, so a consumer who pinned `gssk@1.0.0` from npm was reading a header out of `node_modules` and getting the current one, or pinning a version that never described the kernel it was served. GIP-0001 was written this way: it quotes `INTERACTION /**< Multiplier flow (k * Q1 * Q2) */` and `GetStateSize /* Number of storage nodes */`, neither of which has been the comment for several majors.

  `package.json` is now `5.0.0`, `scripts/release.sh` bumps it alongside the header (and re-reads the file to confirm it is still valid JSON before committing), and `scripts/check_version_sync.py` fails the build when the two disagree — or when `GSK_VERSION_STRING` disagrees with the `GSK_VERSION_MAJOR`/`MINOR`/`PATCH` macros beside it. It is stdlib-only, runs as `make check-version`, and is a prerequisite of `make test`, so a skew cannot survive a local test run.

### Documentation

- **The schema now says where limit logic's saturation constant C comes from.** `F = k × Q_origin / (1 + Q_origin/C)` has been implemented since the primitive was added and the formula is stated in `include/gssk.h`, but nothing told a consumer how to *supply* C. `gssk.schema.json` described `control_node` as a node that "modulates the flow" without saying it **is** the denominator constant, and described `threshold` for threshold and ratio logic only, never mentioning that it doubles as C when no control node exists. Raised as GIP-0001 G6.

  Both `EdgeParams` descriptions and the `EdgeLogic` enum now state the rule: `control_node`'s current Q supplies C if that node is named, otherwise `params.threshold` when it is above zero, and **`control_node` wins when both are given** — `threshold` is the source used in its absence, not a runtime fallback.

  Two behaviours that were not written down anywhere are now stated, in the schema and in the header:

  - A C taken from `control_node` is a **state variable, not a constant.** If that node is itself a store, the edge has a moving half-saturation point.
  - When C falls to `1e-9` or below the flow becomes **exactly 0.0 rather than an error.** A control node that decays toward zero therefore closes the pathway mid-run, in a model that loaded without complaint and whose file says nothing about it. That is deliberate — a saturation constant of zero means the pathway saturates at zero throughput — but it was invisible.

  The GIP filed the second point as "flow is silently 0.0 rather than an error". That is only half true, and the half matters: a limit edge with **neither** source of C is rejected at load with `Logic Error: Edge N (limit) requires control_node or threshold > 0`. Only a C that was valid at load and decayed afterwards reaches the solver.

  `threshold` logic's comparand is documented too: always `Q_origin`, never the control node, and the comparison is strict.

- **`GSSK_LIMIT_C_EPSILON`** is now a named public constant carrying that explanation, following the `GSSK_RATIO_EPSILON` precedent. The eight bare `1e-9` literals in the limit paths use it, so the documentation cannot drift from the threshold it documents. (The loop-limited node's `node_C` guard is a different constant with a different fallback and is untouched.)

- **`tests/test_limit_logic.c`** pins all four facts, so the documentation stays true. The precedence assertion is mutation-tested: swapping `control_node` and `threshold` priority in the kernel makes it fail. The decaying-control test asserts `Q_origin` is *exactly* frozen afterwards, and separately that the origin still holds most of its contents — otherwise a fully-drained store would sit still and pass.

## [5.0.0] - 2026-08-26

### Breaking

GSSK now **rejects models it previously accepted**. Both changes are listed in full below; they are collected here because they are the reason this is a major bump and not `4.2.0`.

- **An unrecognised model key is rejected** instead of silently ignored — `GSSK_Init` returns `GSSK_ERR_SCHEMA_VIOLATION` where it used to return `GSSK_SUCCESS`. Now enforced at every level of the model, not the five originally named.
- **An unrecognised node `type` is rejected** instead of silently becoming a `storage` node.

Neither accepts anything `gssk.schema.json` ever declared valid, so a model that validates against the published schema loads unchanged. What breaks is a model carrying a typo'd or extra key that the parser used to swallow — including, specifically, a node whose `type` was misspelled and which has therefore been silently simulated as a `storage` node, possibly for a long time. That is the case worth checking on upgrade: the error is the first time GSSK has ever told you about it.

**To upgrade:** run your models through `GSSK_Init` and read the messages, which name the offending key directly:

```
Schema Error: Node 'A' has unknown key 'bogus_key'.
Schema Error: Node 'A' has unknown type 'stroage'.
```

`make test-schema` validates a model corpus against the schema without running it.

### Added

- **`GSSK_EnsembleResult` no longer crosses the WASM boundary as a raw struct.** `GSSK_EnsembleForecast` returned a pointer and nothing could read it safely from JS, so every consumer decoded the fields by hand. `web/index.html` did it through `HEAPU32`, under a comment admitting it had gone and read `src/advanced.c` to find the `s * node_count + n` stride; a downstream user independently pinned the same offsets in a golden test, having concluded the layout was an undocumented ABI.

  Five flat accessors close it, following the `GSSK_GetCarrierID` / `GSSK_GetCarrierUnit` precedent that already exists for `GSSK_Carrier`: `GSSK_GetEnsembleNodeCount`, `GSSK_GetEnsembleStepCount`, `GSSK_GetEnsembleMin`, `GSSK_GetEnsembleMax` and `GSSK_GetEnsembleMean`. They take `(step, node)` and apply the stride internally, so it lives in the kernel once instead of in every caller. All five are exported to WASM and declared in `src/gssk.d.ts`; `web/index.html` now goes through them and its pointer arithmetic is gone.

  **Hand-decoding was never portable.** The fields sit at `0/4/8/12/16` under wasm32 but `0/8/16/24/32` in a native 64-bit build, where `sizeof(GSSK_EnsembleResult)` is 40 — any pinned-offset reader is wrong on one of the two targets, and `-sMEMORY64` moves them again.

  **The semantics were documented in `include/gssk.h` the whole time**, and are now harder to miss. The three arrays are pointwise statistics *across runs* — min, max and mean — not three sampled trajectories, so `min <= mean <= max` holds everywhere by construction. The downstream report that two of the arrays "exchange places from sample to sample" was ties, not disorder: a constant node ties at every step, and step 0 of every node ties because perturbation only touches edge `k`. On `examples/economic_model.json` that is 3,006 of 6,006 points. `test_ensemble_getters` in `tests/test_advanced.c` asserts the ordering pointwise, asserts the getters agree with the fields they expose, pins both tie classes, and checks that a non-constant node genuinely spreads so the ordering assertions cannot pass on a degenerate ensemble.

- **Phase E.1 — the countercurrent is a first-class example, with an annotated twin.** C.4 shipped `examples/odum_countercurrent.json` carrying its own commentary inside `_`-prefixed fields. That is the wrong file for it: every other reference model here is a plain model beside a separate annotated twin (`household_model.json` / `household_model_annotated.json`), and someone reading for the topology should not have to read four essays to find thirteen nodes. The prose now lives in `examples/odum_countercurrent_annotated.json`, and the plain file is the model alone.

  **The golden CSV did not change by a single byte.** That is the evidence that this was a re-shelving and not an edit — `tests/expected/odum_countercurrent.csv` is bit-for-bit what C.4 generated, and `make test-net-energy` still asserts the same claim against the stripped file.

  `make test` now also checks **annotated twins against the models they document**. Each `X_annotated.json` gets its own golden CSV, byte-identical to `X.csv`, and the harness compares the two trajectories directly. Golden CSVs alone cannot catch this class of drift: edit the annotated model, run `make test-update`, and its golden is regenerated to match the model that has just diverged, so everything passes while the documented model and the running model are different systems. Verified by negative control — perturbing the exchange `k` in the annotated file to `0.0031` and regenerating its golden passes every per-model comparison and fails the twin check.

  The fuzz corpus gains `tests/fuzz_corpus/seed_closed_money_loop.json`: money circulates `buyer → exchange → till → buyer` with no money source and no money sink, so the parser is seeded with the closed-loop topology `d1` will build on, and money is conserved over the run. `seed_exchange_node.json` stays, as the open-loop counterpart.

- **Phase C.4 — inflation emerges from net-energy decline.** `examples/odum_countercurrent.json` builds Odum's Figure-2 structure: the fuel reserve is a depleting **storage**, extraction is a work gate needing both the reserve and the machinery, and the energy spent *getting* energy is fed back as a cost inversely proportional to what is left. As the reserve depletes the feedback fraction rises, the net energy reaching the economy collapses faster than the gross, and price — money over real throughput — rises.

  Measured over the run: fuel `1000 → 20`, structure booms `1 → 391` by `t≈29` then busts to `22`, net-energy-per-gross falls `0.85 → 0.27`, and the price index rises **`25 → 18,708`, a 739× inflation**, monotonically across the whole depletion phase.

  **The money supply is exactly constant for the entire run** — `buyer` is a `source`, so `compute_derivatives` pins it — and `make test-net-energy` asserts that at every step. That is the control that makes the claim mean something: with `M` fixed, the rise in `P = M/W` is attributable to `W` alone and cannot be a monetary effect. This is Odum (1973) points 1–3, and it is the piece that makes inflation emerge from net-energy decline rather than from supply and demand.

  **No kernel change was required, which was not expected.** The task was filed against `src/gssk.c`. What unblocked it was C.3: `params.numerator_node` ([ADR 0005](adr/0005-price-relaxation-and-named-numerator.md)) lets a `ratio` edge name its numerator instead of taking it from the origin, so the gross-energy tap can read `k·Q_yield/Q_fuel` from a **pinned** origin without consuming either node. Without it that tap would have had to originate at `yield` and would have double-debited the very flow it exists to measure — the hazard ADR 0003 named and ADR 0005 closed.

  `make test-net-energy` asserts the **claim**, not a trajectory — the golden CSV in `make test` already pins the digits and can say nothing about whether the mechanism is the one described. It checks boom/bust, one-way depletion, the feedback fraction rising against its closed form, monotone net-per-gross and price across the depletion phase, the pinned money supply, and that price *tracks* `M/W` (worst departure 4.3%, the relaxation lag) rather than merely correlating with it. Both halves are verified by negative control: breaking the `α` equality produces a 51% departure, and removing the depletion feedback holds net/gross at exactly `1.0000`.

- **Forcing functions — Odum's eleven, as one waveform vocabulary attached in two places.** A source node held its declared value for the whole run, so GSSK expressed exactly two of the eleven forcing functions in *Systems Ecology* Fig. 7-2 (constant force and constant flow) and had no representation for the other nine.

  `forcing` on a **node** drives its held value (Odum X/N, a *force*); `forcing` on an **edge** drives its rate `k` (Odum J, a *flow*). Eight waveforms: `step`, `impulse`, `ramp`, `sawtooth`, `square`, `sine`, `exponential`, `jitter`. Odum's eleven are a node-value-versus-edge-rate distinction crossed with a carrier distinction, and carriers were already modelled — so eleven node types would have been the wrong shape. [ADR 0006](adr/0006-forcing-one-vocabulary-two-attachments.md) records that decision and what was rejected. Worked model in `examples/forced_source_model.json`; full vocabulary and formulas in [docs/concepts.md](concepts.md#forcing-functions).

  **Waveforms are evaluated at solver STAGE times, not once per step.** This is the requirement that is easy to get wrong and invisible when you do: sampling once per step leaves the forcing first-order while the state is fourth- or fifth-order, and the run still completes and still looks smooth. Integrating a sine-forced source against its closed form, the error ratio per halving of `dt` is **16.02× / 16.01× / 16.00×** with stage times and **1.97× / 1.98× / 1.99×** without. Every other test in the suite passes either way. `h8a-thread-time-through-ode-core` landed first to make the stage times available.

  **`jitter` is latched once per accepted step**, drawn from the instance SplitMix64 stream (`GSSK_SetSeed`), never libc `rand()`. A per-stage draw would make the trajectory depend on solver internals — the same model answering differently under `rk4` and `dopri5` for reasons that are not physics. The test asserts the *draw sequence* is bit-identical across `rk4`, `incipient` and `adaptive`.

  **A storage node cannot be forced** — its value is the integral of its flows, so forcing it asserts two things about one quantity. `GSSK_Init` and `GSSK_AddNode` both reject it naming the node, rather than ignoring the block, which is the failure mode `h8b` was landed to remove. A periodic waveform without a positive `period` is likewise rejected rather than silently treated as a constant.

  **`phase` is a time offset, not an angle** — same units as `t`, and *subtracted*, so a positive phase delays the waveform. **`impulse` is area-normalised** over one nominal `dt`, so its integral is `area` at any step size. Both conventions are stated in the header, the schema, `docs/concepts.md` and the example, because an ambiguous convention is how two implementations diverge while both look right.

  New accessors, all exported to WASM and typed in `src/gssk.d.ts`: `GSSK_GetNodeForcingKind`, `GSSK_GetEdgeForcingKind`, `GSSK_EvaluateNodeForcing`, `GSSK_EvaluateEdgeForcing`. They are **the same evaluator the derivative path uses** — a test drives each of the eight waveforms and asserts what the evaluator reports equals what the kernel integrated, so the two cannot drift. Flat scalars rather than a struct pointer, for the reason the flat carrier getters exist. `js/gssk.js` gains the matching wrappers.

  Forced node values are written into the live state after each step, so `GSSK_GetState` and the CSV show the waveform. Previously the derivative was correct while a sine-forced source appeared as a flat line next to the storage it was visibly driving.

### Changed

- **`GSSK_Reset` does not rewind the random stream, and now says so.** Behaviour is unchanged; the contract is newly documented because forcing makes it reachable. Rewinding was tried and is wrong: `GSSK_EnsembleForecast` and `GSSK_CalibrateMonteCarlo` perturb with the instance RNG and then reset once per run, so rewinding collapses an ensemble to one trajectory (`test_advanced`'s calibration caught it). `GSSK_Reset` means "back to `t_start`", not "back to the start of the stream". To repeat a `jitter` run exactly, call `GSSK_SetSeed(inst, GSSK_GetSeed(inst))` first.

### Known limitation

- **Trajectories are not bit-identical across platforms**, because the kernel uses the platform's `libm`. Measured three ways on the same eight sample points: the WASM build agrees with Linux GCC **exactly**, and macOS Apple clang's `sin()` is the outlier, differing by 1 ULP at two of the eight. `sin`/`exp`/`pow` are not required by IEEE-754 to be correctly rounded, so this is inherent rather than a defect in any toolchain. It pre-dates forcing — `exp()` was already on the Riccati duet path and `pow()` on the adaptive step controller — but forcing makes it common, since a sine-forced model hits a transcendental on every stage of every step. `make test-forcing-wasm` therefore asserts agreement to 4 ULP rather than bit-equality, with the measurement recorded in the check itself. Whether GSSK should ship its own correctly-rounded transcendentals is a Phase G reconstruction question, tracked as `deterministic-transcendentals-cross-platform`.

### Changed

- **Simulation time is now threaded through the ODE core.** `compute_derivatives` takes an explicit `double t`, and every call site passes the correct *stage* time: classical RK4 at `t`, `t+h/2`, `t+h/2`, `t+h`; DOPRI5 at `t + c_i·h` for `c = (0, 1/5, 3/10, 4/5, 8/9, 1, 1)`. The whole derivative surface is covered, not just RK4 — both `rk4_step` variants, DOPRI5's seven stages, the IDC path (`build_flow_matrix`, `build_forcing_vector` and the processing-node helpers), `build_jacobian`, `compute_param_deriv`, `compute_quality_pass`, `compute_quality_sensitivity`, threshold sub-stepping, and the adjoint's backward integration.

  **Nothing consumes `t` yet, and every trajectory is bit-identical.** `tests/expected/` is untouched and `make test` passes against it unchanged — that is the acceptance criterion, not a side note. Landing the threading on its own means any later trajectory change is attributable to the feature that uses `t` rather than to a mistake in threading it through 16 call sites and seven solver stages.

  The DOPRI5 c-nodes were *already written down* — `/* Stage 2 at c2 = 1/5 */` and so on — and thrown away, because there was no `t` for them to offset. They are now used rather than described.

  This exists for forcing functions, and it is the trap that feature would otherwise fall into: a waveform sampled once per **step** instead of once per **stage** is first-order while the state is fourth- or fifth-order. The run completes, the trajectory looks smooth, and the order loss is invisible without a convergence study.

  `make test-stage-times` records the time the solver actually hands each derivative evaluation and asserts the sequence, rather than re-deriving the arithmetic in the test — which would just be the same mistake written twice. It covers the two sharp cases: adaptive sub-stepping, where stage 1 of sub-step 2 must be at `t + h₁` and not `t`, and the adjoint, which runs time backwards and must land on `t_start`. The recorder is compiled in only under `-DGSSK_STAGE_TIME_PROBE`, which only that target defines; it is not in `gssk.h`, not in the shipped library, and not in the WASM export list. **No public API change and no schema change.**

### Added

- **Flat carrier accessors that carry no struct layout across the WASM boundary.** `GSSK_GetCarrierID`, `GSSK_GetCarrierUnit`, `GSSK_GetCarrierConserved` and `GSSK_FindCarrierIdx`, all four exported to WASM and typed in `src/gssk.d.ts`.

  The data was always reachable from C — `GSSK_GetCarrier` returns the whole `GSSK_Carrier`. The gap was JS: across WASM that same call is a bare heap pointer, so reading `unit` or `conserved` meant assuming field offsets, the width of `bool` and the absence of trailing padding, none of which is an ABI contract and all of which break by returning plausible garbage rather than by failing. `unit` is also the y-axis label a plotting consumer would otherwise hardcode, and `unit` plus `conserved` is what a consumer needs to decide two series may not share a scale (ADR-6, ADR-8).

  `GSSK_GetCarrier` is unchanged and stays for C consumers. The out-of-range conventions differ deliberately and are documented on both: the string getters return `""` and never `NULL`, following `GSSK_GetNodeCarrier`, while `GSSK_GetCarrier` still returns `NULL`; `GSSK_GetCarrierConserved` returns `0`, which is indistinguishable from a declared non-conserved carrier, so bound-check against `GSSK_GetCarrierCount` first; `GSSK_FindCarrierIdx` returns `-1`, matching `GSSK_FindNodeIdx` / `GSSK_FindEdgeIdx`. `make test-carrier-api` covers each getter, every out-of-range path, the lookup round-trip, and — the check that matters — that the flat path and `GSSK_GetCarrier` agree at every index, so the two cannot drift.

- **Phase C.3 — price is now a state variable that relaxes toward Odum's ratio.** `dP/dt = α(M/W − P)`, with `α` a settable per-model adjustment rate rather than a hard-coded one. `examples/price_dynamics_model.json` wires circulating money `M` and the C.2 delivered-work signal `W` into the C.1 `ratio` primitive and feeds the result back into the transaction diamond through the C.0 `price_node` hook. The fixed point is `M/W` itself, not something proportional to it: `make test-price-dynamics` asserts convergence to `M/W` to 1e-9 and the approach to `(M/W)(1 − e^{−αt})`, so both the level and the time constant are hand-checked rather than golden. `examples/emergent_price_model.json` is unchanged and stays in the suite, so the difference between the Tier 1 proportional anchor and the Tier 2 true ratio remains visible.

  **What this is, precisely:** the *imposed* transactor with a dynamically computed price. Odum draws two price notations — one where the transaction generates price (*Systems Ecology* Fig. 23-3c) and one where price is determined outside and imposed on the pathway (Fig. 23-3d) — and GSSK's exchange node is the second: `F_money = P × F_goods`. His endogenous price is a ratio of two independently driven *flows*, `p₁ = J₁/J₄` (Fig. 23-2c), which cannot be reached by measuring this diamond's own flows, since `J_money := P·J_goods` makes the quotient an identity. `M/W` is dimensionally a price (AUD/kg) and proportional to `J₁/J₄`, with the constant set by β. So price here is endogenous in where the number comes from and exogenous in how it acts on the trade — a real and common configuration, and the one the task specified, but not the price-generating transactor. [ADR 0005](adr/0005-price-relaxation-and-named-numerator.md) records both the relaxation-over-algebraic decision and this fidelity boundary in full.

- **`ratio` edges accept `params.numerator_node`.** The numerator is now an optional *named* operand, read by id and not consumed — the same contract `control_node` has had as the denominator. Omitted, the numerator is `Q_origin` and behaviour is bit-for-bit unchanged.

  This exists because an edge debits its origin, so before it the only way to put a stock in the numerator was to drain it: a price mechanism reading `M` would have eaten the money supply it was observing. ADR 0002 called for a ratio whose numerator and denominator are "both named and distinguishable" and then named only the denominator; this supplies the other name. The edge remains a flow from origin to target — pin that origin with a `source` or `constant` node when the flow must cost nothing, as [ADR 0003](adr/0003-delivered-work-signal.md) established for the `W` tap.

  An unknown `numerator_node` id is a linkage error, and `numerator_node` on any logic other than `ratio` is a logic error rather than a silently ignored key.

### Fixed

- **Deactivation did not survive serialise → reload.** `GSSK_DeactivateEdge` clears `active` *and* sets `k` to `0`, and `GSSK_Init` accepted the emitted `"active": false` without acting on it — so a round-trip produced an edge that was **active with `k = 0`**, not an inactive edge. The trajectory matched either way, which is why it survived: `k = 0` kills the flow regardless, so the only thing that differed was the flag.

  The flag is not decorative. It is read at roughly twenty sites in `src/gssk.c`, and the ones that matter **count elements rather than sum flows** — which is exactly what `k = 0` cannot stand in for. `network_is_isolated_duet` requires exactly one active edge before the Riccati closed form is used, motif detection skips inactive nodes and edges, and the closed-system conservation check sums only active nodes. A reloaded model could therefore be treated differently from the model it was serialised from while producing identical numbers.

  **The node half was worse and was lost outright.** `build_topology_json` emitted no `active` for nodes at all, and a node has no `k` to carry the deactivation the way an edge accidentally did, so `GSSK_DeactivateNode` did not survive in any form. `Node.active` is now emitted (only when false), declared in `gssk.schema.json` and listed in `NODE_KEYS`.

  **The flag is never inferred from `k`.** An edge authored with `k: 0` is present and carrying nothing; a deactivated edge has been taken out of the network. Reading the flag back from the conductance would reproduce the trajectory and lose the topology — the same defect one level down. `tests/schema_fixtures/deactivated_elements.json` pins both directions, and puts an `active` key into `tests/results/serialized/` for the first time: that corpus never contained one, which is why the original defect went unseen.

  `make test-deactivation` (`tests/test_deactivation_round_trip.c`) asserts on motif count and on what happens when `k` is restored — an inactive edge stays dead, an authored-zero edge starts flowing — because a trajectory assertion cannot distinguish the two by construction. It fails eight assertions against the pre-fix kernel. `GSSK_AddNode` and `GSSK_AddEdge` honour `active` too, being separate parsers. [ADR 0004](adr/0004-schema-advisory.md) is amended with the closure, including a correction: the flag is *not* read by `GSSK_ReclassifyNetwork`, which inspects no edges at all.

- **A `ratio` edge serialised as a `linear` edge.** `logic_type_str` was never given a `ratio` case and fell through to its `"linear"` default, so every round-trip through `GSSK_SerializeModel` or `GSSK_SerializeSnapshot` silently replaced the division with a proportional flow — permanently, and with no error. This reached snapshots and the Phase G archival dumps, where the serialised form *is* the artefact. ADR 0002 tabulated the eight sites a new logic type must be added to; the serialiser was not among them, which is how it was missed.

- **A `ratio` edge's denominator floor did not survive serialisation.** `params.threshold` is the floor override for `ratio` logic, but it was emitted only for `threshold` logic. A deliberate floor of `0.01` round-tripped back to `GSSK_RATIO_EPSILON` (1e-9) — a 1e7× change in the price a model reports once `W` empties.

### BREAKING

- **Unknown-key rejection now covers the whole model, not the five levels `h8b` named.** `GSSK_Init` returns `GSSK_ERR_SCHEMA_VIOLATION` for an unrecognised key in node `params`, `metadata`, a `carriers[]` entry, `snapshot` (and its nested `state[]`, `edge_k[]`, `solver`, `rng_state` and `mutation_log[]` objects), an `archetypes` template (and its node, edge and `params` objects), and the root-level `mutation_log[]`. Messages name the key **and** its container, as before — `Schema Error: Node 'mkt' params has unknown key 'pric'.`, `Schema Error: metadata has unknown key 'authorr'.`, `Schema Error: Carrier 'energy' has unknown key 'unti'.`, `Schema Error: Archetype 'widget' edge 'bleed' has unknown key 'origen'.`

  **Node `params` is the sharpest of these** and is the direct analogue of edge `params`, which `h8b` did close. `{"type":"exchange","params":{"pric":10}}` used to load, ignore `pric`, and run the transaction at the default price — the same "plausible completed run, quietly different model" failure `h8b` exists to prevent, on the parameter a Phase C price model turns on. `GSSK_AddNode` applies it too, being a separate parser; `node_keys_ok` is shared between the two.

  **`metadata` matters for a different reason**: it carries `model_hash`, which the kernel round-trips and never computes. A typo'd provenance key was silently dropped from an artefact whose whole purpose is provenance.

  **`^_` keys are accepted at every new level**, as everywhere else. Every one of these levels was **already** declared with `additionalProperties: false` and `^_` `patternProperties` in `gssk.schema.json`, so this is the same argument as `h8b` — bringing the kernel into agreement with a contract the project already publishes — not a new rule.

  **Two things the sets record rather than paper over.** `snapshot.dt` is **emitted by `GSSK_SerializeSnapshot` and never read back**: reload takes `dt` from `config`, as it does for a model with no snapshot at all. Deriving `SNAPSHOT_KEYS` from the parser alone would have omitted it and rejected every snapshot the kernel has ever written — the drift direction that breaks *working* models, which is worse than the bug being fixed — so it is in the set with a comment saying why, and has its own assertion. Separately, `archetypes` node templates are declared with the full `NodeParams` `$def` while `parse_user_archetypes` reads only `k`, `C`, `threshold` and `price`; the wider published set is honoured, because narrowing the kernel to the parser would reject models the schema calls valid.

  **Migration**: identical to `h8b`'s. A model carrying a stray key at any of these levels now fails to load; correct the key or `_`-prefix it. Nothing in `examples/`, `tests/schema_fixtures/` or `tests/results/serialized/` changed — all 21 loadable models still load and all 42 serialised artefacts still validate. `make test-unknown-keys` covers each new level with one rejection naming key and container, one `_`-prefixed acceptance, and the corpus regression; it fails 30 assertions against the previous kernel. [ADR 0004](adr/0004-schema-advisory.md) is amended with the closure.

- **An unrecognised model *key* is now rejected instead of silently ignored.** `GSSK_Init` returns `GSSK_ERR_SCHEMA_VIOLATION` for a key it does not recognise at the root object, in a node object, in an edge object, in edge `params`, or in `config`. The message names both the key and its container so an authoring UI can highlight the element that is wrong — `Schema Error: Edge 'e1' params has unknown key 'bogus_param'.`

  The hazard is sharpest for a feature the kernel does not yet have. This model used to load, return `GSSK_SUCCESS` and run to completion with `forcing`, `nonsense_top_level_key` and `bogus_param` all ignored in silence:

  ```json
  {"metadata":{"schema_version":4},
   "forcing":{"sun":{"waveform":"sine","amplitude":5,"period":24}},
   "nonsense_top_level_key":123,
   "nodes":[...], "edges":[{"...":"...","params":{"k":0.5,"bogus_param":9}}]}
  ```

  A model authored against a kernel that *has* forcing, loaded by one that does not, therefore produced a plausible completed run with no diagnostic. Its JSON — and any external content hash of it, which is what `metadata.model_hash` carries, since the kernel round-trips that field and never computes it — says "forced". Its trajectory says "constant". Nothing reconciles the two. This is the same hazard class as the node-`type` fallback below: a wrong key is not a smaller mistake than a wrong type, it is the same mistake one level up.

  **Any key beginning with `_` is accepted everywhere, at every level.** That is load-bearing rather than a courtesy: `examples/household_model_annotated.json` and `examples/price_dynamics_model.json` carry `_note` and `_mechanism` throughout, and `gssk.schema.json` documents the convention.

  `GSSK_AddNode` and `GSSK_AddEdge` apply the same check — they are separate parsers. A rejected add is a true no-op: the check runs before the `realloc`, so counts are unchanged and the instance is still steppable.

  **This makes the kernel agree with a contract the project already publishes.** `gssk.schema.json` has always set `additionalProperties: false` at the root and on `Node`, `Edge`, `EdgeParams` and `Config`, and permitted `^_` keys via `patternProperties`. `make test-schema` caught violations in `examples/`; nothing caught them at runtime for a *consumer's* model, which is where it matters. See [ADR 0004](adr/0004-schema-advisory.md).

  **Migration**: a model carrying a stray key now fails to load; correct or `_`-prefix the key. Nothing in `examples/`, `tests/schema_fixtures/` or the fuzz corpus changed — all 18 loadable models still load, and the two fuzz seeds that fail still fail for their original, unrelated reasons (one is not JSON, one has no `nodes` array).

### Fixed

- **The serialiser emitted `"active"`, a key the published schema forbids.** `build_topology_json` writes `"active": false` for an edge deactivated via `GSSK_DeactivateEdge`, but `active` appeared nowhere in the parser and `Edge` sets `additionalProperties: false` — so the kernel was emitting output its own schema rejects. It went unnoticed because no model in `examples/` or `tests/schema_fixtures/` has a deactivated edge, so the `tests/results/serialized/` corpus never contained one. Found by the stricter parser above, which is exactly what it is for. `active` is now declared in the schema and accepted on load, and `tests/test_unknown_keys.c` covers the deactivated-edge round-trip directly.

  Note what is *not* fixed here: `GSSK_Init` accepts `active` but does not act on it. Deactivation survives the round-trip through `params.k`, which `GSSK_DeactivateEdge` sets to `0.0`, so the trajectory is reproduced — but `edges[i].active` is not, and that flag is read by topology classification, so a reloaded edge is *active with k = 0* rather than *inactive*. That is a behavioural change and is tracked separately.

### BREAKING

- **An unrecognised node `type` is now rejected instead of silently becoming a `storage` node.** `parse_node_type` returned `NODE_STORAGE` for any string it did not recognise, so `"storge"`, `"Source"` or `"producer_"` produced a *different model* that ran to completion and reported success. `GSSK_Init` now returns `GSSK_ERR_SCHEMA_VIOLATION` for any type that is neither one of the nine primitives (`storage`, `source`, `sink`, `constant`, `interaction`, `gain`, `loop_limited`, `exchange`, `switch`), a built-in composite (`producer`, `consumer`, `misc_box`, `system_frame`), nor an archetype declared in the model's own `archetypes` block. The message names the node id and the offending string — `Schema Error: Node 'grasss' has unknown type 'storge'.` — so an authoring UI can highlight the element that is wrong.

  `GSSK_AddNode` rejects the same strings, and additionally rejects composite and archetype names: it performs no expansion, so `{"type":"producer"}` added at runtime had been becoming a single storage node rather than the producer subgraph. It now fails with a message saying composites can only be added at `GSSK_Init`. A rejected add is a true no-op — nothing is allocated or grown before the check, so the instance a drag-and-drop editor is mutating is left exactly as it was and remains steppable. Expanding composites at runtime is a separate change and is not attempted here.

  **Migration**: a model relying on the old fallback now fails to load. The fix is to correct the type string; every previously-accepted string that was actually a primitive, a built-in composite, or a declared archetype is unaffected. Nothing in `examples/`, `tests/schema_fixtures/` or the fuzz corpus changed.

  This closes the hazard [ADR 0004](adr/0004-schema-advisory.md) left open, using the fix that ADR named. Schema validation could never have caught it: `Node.type` cannot be a closed enum, because archetype names are user-defined, so a validator cannot tell a typo from a legitimate archetype reference. The parser can — it reads the `archetypes` block before the node list — which is why the check belongs there and why the schema can stay advisory.

### Fixed

- **`docs/api-reference.md` documented `GSSK_Carrier.id` as `id[64]`.** It is and always has been `char id[32]`. Harmless in C, but the API reference is exactly where a JS consumer would go to work out the struct offsets to decode by hand — a reader who trusted it would have read `unit` 32 bytes past where it lives. Noticed while writing up the flat getters; corrected, and the reference now tells JS not to decode the struct at all.

- **`GSSK_AddNode` read freed memory when reporting a duplicate id.** The error message formatted `id->valuestring` after `cJSON_Delete` had already released the tree that owned it. Found while adding the type check next to it; the message is now formatted before the delete.

- **GCC 11 can build the kernel again.** `append_mutation` used `strncpy` followed by an explicit terminator, a pattern GCC's `-Wstringop-truncation` rejects under `-Werror` even though it is correct. Every `strncpy` in `src/gssk.c` (107 sites) is now `safe_str_copy`, which always NUL-terminates and derives its bound from `sizeof(dst)` so the bound cannot drift from the field width. This removes the limitation recorded against 4.1.0.

- **Schema conformance is now tested.** `make test-schema` (and `make test`) validates three corpora against `gssk.schema.json` and fails the build on a mismatch: the hand-written models in `examples/`, the corner-case fixtures in `tests/schema_fixtures/`, and — via the new `bin/dump_serialized` — the JSON that `GSSK_SerializeModel` and `GSSK_SerializeSnapshot` actually emit for every one of them. The schema and the parser can no longer drift apart unnoticed in either direction. CI installs `jsonschema` to make the gate real; locally it skips with a message when the dependency is absent. See [ADR 0004](adr/0004-schema-advisory.md) for why the schema stays advisory rather than being enforced inside `GSSK_Init`.

- **Fixed: the schema rejected the kernel's own adaptive-solver config.** `config.rel_tol`, `abs_tol`, `h_min` and `h_max` are read by `GSSK_Init` and written back by `GSSK_SerializeModel`, but `Config` did not list them and set `additionalProperties: false` — so any model using DOPRI5 tolerances, including one the kernel had just serialised, failed validation against the project's own schema. All four are now described. The root-level `mutation_log` block is also documented for what it is: an archival copy, which `GSSK_Init` restores only from `snapshot.mutation_log`.

---

## [4.1.0] — 2026-08-17

The first release published under an immutable version tag. Everything below shipped since 3.6.0; the intervening 4.0.0 was distributed only through the rolling `latest` pre-release and was never tagged or recorded here. Consumers who pinned artifacts from that rolling tag should move to `v4.1.0`, which differs from it — this release adds nine exported functions and changes how the stochastic entry points draw randomness.

### Added

- **Phase 7 — complete ESL node type taxonomy.** All seven Odum symbols are now implemented: `interaction`, `gain`, `loop_limited`, `exchange` and `switch` join `source`, `storage`, `sink` and `constant`. Processing nodes are configured through the node's `params` block (`k`, `C`, `threshold`, `price`) rather than through edge parameters, consistent with the ESL topology rule. Schema v4 with a `gssk migrate --from 3` path.
- **Phase 8 — composite node types and archetypes.** Built-in `producer`, `consumer`, `misc_box` and `system_frame` composites expand at `GSSK_Init` time into namespaced primitives (`{instance}__{member}`). User-defined templates may be registered via a top-level `archetypes` block and used as node types. Ports define external attachment.
- **Phase 9 — runtime pattern discovery.** Recurring 2–3 node subgraph motifs are detected after each step and, once stable, promoted to named archetypes via `GSSK_ProposeArchetype`. `GSSK_GetGenerativityIndex` reports the rate of emergence.
- **Composite membership API** (GH #29 item 1): `GSSK_GetNodeComposite`, `GSSK_GetNodeRole`, `GSSK_GetCompositeMemberCount`, `GSSK_GetCompositeMemberIndex`, `GSSK_GetCompositeArchetype`. Membership is recorded during expansion, so consumers no longer have to infer it by string-matching the `{instance}__{member}` prefix — an inference that is unsound in both directions and silently corrupts aggregation.
- **Seedable randomness** (GH #29 item 5): `GSSK_SetSeed`, `GSSK_GetSeed`, `GSSK_NextRandom`, `GSSK_NextRandomUniform` and `GSSK_DEFAULT_SEED`. Snapshots now carry `{seed, state}` in place of the previous null placeholder.
- **Containerised Linux toolchains**: `make wasm-container`, `make test-linux`, `make test-linux-clang`, `make ci-local` build WASM and run the suite under real GCC from macOS.
- **Whitepaper and article** under `doco/`, built with `make doco`.

### Changed

- **Stochastic entry points are now reproducible.** `GSSK_EnsembleForecast` and `GSSK_CalibrateMonteCarlo` draw from an instance-owned SplitMix64 generator seeded at init instead of libc `rand()`. Same model plus same seed now gives bit-identical results across platforms and under WASM. **This is a behavioural change**: `srand()` no longer influences either function, so callers that relied on it must use `GSSK_SetSeed`.
- **`gssk.schema.json` regenerated for v4** (GH #29 item 2). The published schema rejected models the kernel accepts. Beyond the missing `archetypes` block and node types, it also required `logic` and `params.k` on every edge, had no node `params` block, declared `snapshot` as a closed empty object so no serialised snapshot could validate, and rejected the `_`-prefixed annotation convention used in the bundled examples. `Node.type` is now an open string, since user archetype names are open-ended and unrecognised types are not rejected by the kernel.
- **`docs/concepts.md` corrected** (GH #29 item 3). It described composites as future work although they shipped, and the composite table was wrong on the facts. The same stale future tense applied to Phase 7 and Phase 9.
- **Releases now publish immutable version tags** (GH #29 item 6) with `gssk.schema.json` attached alongside `gssk.js`, `gssk.wasm` and `gssk.d.ts`, and a SHA-256 table in the release notes. The rolling `latest` pre-release continues, now labelled as republished in place.
- **CI builds WASM on pull requests.** Previously only the deploy job built WASM, so export changes were unverified until after merge.

### Fixed

- `GSSK_AddNode` zeroed the node struct, which would have made every runtime-added node report membership in composite 0.
- Monte Carlo calibration selected population indices with `rand() % n`, biasing toward low indices; now rejection sampling.
- `src/gssk.d.ts` reported the schema version range as "2 or 3" (GH #29 item 4).
- The release workflow force-pushed the bare version tag onto an orphan dist commit lacking `Package.swift` and `src/`, which would have broken SPM resolution and made every version tag mutable. The dist tree now uses a `dist-vX.Y.Z` namespace.

### Known limitations

- An unrecognised node `type` is not rejected — the kernel falls back to `storage`, so a typo yields a silently incorrect model. Validate against `gssk.schema.json` before calling `GSSK_Init`.
- `system_frame` is structural only: it reserves a name but expands to no subgraph. The ESL switching-box composite is unimplemented.
- Structural capacities are fixed at compile time: 32 archetypes, 128 composite instances, 16 nodes and 32 edges per archetype.
- Motif detection is skipped above 64 nodes.
- Building with GCC 11 fails on a `stringop-truncation` diagnostic in `append_mutation`. GCC 13, Clang and Apple Clang are unaffected.

---

## [3.6.0] — Phase 6: Productionisation

### Added
- **CI/CD**: GitHub Actions matrix (`ci.yml`) covering gcc/clang × linux/macos, AddressSanitizer + UBSan, lcov coverage gate (≥ 85%), Valgrind, LibFuzzer (30 s), perf regression, Swift 5.10 + 6.0
- **Python binding** (`python/gssk.py`): full ctypes wrapper with `GSSKSimulator`, `from_file`, sensitivity, carriers, serialisation, optional pandas `run_dataframe()`
- **Python tests** (`python/test_gssk.py`): 31 tests via `make test-python`
- **JavaScript/TypeScript wrapper** (`js/gssk.js`): `GSSKSimulator` ES module with async `create()`, carrier access, mutation log
- **Benchmark suite** (`bench/`): `run_bench.sh` + models at 10/100/1 000 nodes; `make bench` and `make bench-check` targets
- **VitePress docs restructure**: `docs/concepts.md`, `docs/api-reference.md`, `docs/cookbook.md`, `docs/CHANGELOG.md`; sidebar reorganised into Guides / Reference / Examples sections
- **Release script** (`scripts/release.sh`): bumps `GSSK_VERSION` in `include/gssk.h`, tags git
- **SECURITY.md**: vulnerability reporting process
- **LICENSE**: MIT

### Changed
- `Makefile`: added `shared`, `test-python`, `asan`, `test-asan`, `coverage-build`, `coverage-report`, `coverage-check`, `test-valgrind`, `fuzz-build`, `fuzz-run`, `bench`, `bench-check` targets
- WASM exports list updated to include carrier functions

---

## [3.5.0] — Phase 5: Multi-Carrier Networks

### Added
- `carriers` array in schema (v3): `{id, unit, conserved}`
- `GSSK_GetCarrierCount`, `GSSK_GetCarrier`, `GSSK_GetNodeCarrier`, `GSSK_GetEdgeCarrier`, `GSSK_GetCarrierConservationError`
- Swift binding: `GSSKCarrier` struct, `carrierCount`, `carrier(at:)`, `carriers`, `nodeCarrier(at:)`, `edgeCarrier(at:)`, `carrierConservationError(for:)`
- TypeScript declarations for carrier functions in `src/gssk.d.ts`
- Household model (`examples/household_model.json`): 4 carriers × 23 nodes
- Household model documentation (`docs/examples/household/README.md`)
- Annotated household model (`examples/household_model_annotated.json`)
- Jupyter notebook (`examples/household_notebook.ipynb`)
- Browser interactive demo (`docs/examples/household/demo.html`)

### Fixed
- LIMIT edge validation: `threshold > 0` is accepted as the saturation constant without requiring `control_node`

---

## [3.4.0] — Phase 4: Advanced Analysis

### Added
- Forward (tangent-linear) sensitivity: `GSSK_EnableForwardSensitivity`, `GSSK_DisableForwardSensitivity`, `GSSK_GetSensitivity`
- Adjoint sensitivity: `GSSK_RunAdjoint`, `GSSK_GetTransformitySensitivity`
- Gradient calibration: `GSSK_CalibrateGradient`
- Monte Carlo calibration: `GSSK_CalibrateMonteCarlo`
- Ensemble forecast: `GSSK_EnsembleForecast`, `GSSK_FreeEnsembleResult`
- Event detection: `GSSK_GetEventCount`, `GSSK_GetEventTime`, `GSSK_GetEventEdgeID`, `GSSK_GetEventDirection`

---

## [3.3.0] — Phase 3: Structural Mutation

### Added
- Mutation log with `GSSK_GetMutationCount`, `GSSK_ExportMutationLog`, `GSSK_ClearMutationLog`, `GSSK_SetMutationCause`
- Dynamic topology: `GSSK_AddNode`, `GSSK_AddEdge`, `GSSK_DeactivateEdge`, `GSSK_DeactivateNode`, `GSSK_ReclassifyNetwork`
- Replay: `GSSK_Replay`

---

## [3.2.0] — Phase 2: Adaptive Stepping & Serialisation

### Added
- Adaptive RK4 solver: `GSSK_StepAdaptive`, `GSSK_GetLastStepSize`, `GSSK_GetNextStepSize`
- Model serialisation: `GSSK_SerializeModel`, `GSSK_SerializeSnapshot`, `GSSK_FreeString`
- Solver diagnostics: `GSSK_GetSolverConfidence`, `GSSK_GetEdgeErrorEstimate`, `GSSK_GetStepErrorEstimate`
- Diagnostic hooks: `GSSK_SetDiagHooks`

---

## [3.1.0] — Phase 1: Core Kernel

### Added
- C99 kernel: `GSSK_Init`, `GSSK_Step`, `GSSK_Reset`, `GSSK_Free`
- RK4 and Euler integrators
- Edge logic: `linear`, `constant`, `michaelis_menten`, `limit`, `interaction`
- Node types: `storage`, `source`, `sink`
- State/node/edge accessors: `GSSK_GetState`, `GSSK_GetStateSize`, `GSSK_GetNodeID`, `GSSK_GetEdgeID`, etc.
- cJSON bundled parser
- Swift Package: `CGSSK` system module + `GSSK` wrapper
- CLI tool: `bin/gssk <model.json> <output.csv>`
- Regression test suite with CSV comparison

---

[Unreleased]: https://github.com/sholtomaud/GSSK/compare/v3.6.0...HEAD
[3.6.0]: https://github.com/sholtomaud/GSSK/compare/v3.5.0...v3.6.0
[3.5.0]: https://github.com/sholtomaud/GSSK/compare/v3.4.0...v3.5.0
[3.4.0]: https://github.com/sholtomaud/GSSK/compare/v3.3.0...v3.4.0
[3.3.0]: https://github.com/sholtomaud/GSSK/compare/v3.2.0...v3.3.0
[3.2.0]: https://github.com/sholtomaud/GSSK/compare/v3.1.0...v3.2.0
[3.1.0]: https://github.com/sholtomaud/GSSK/releases/tag/v3.1.0
