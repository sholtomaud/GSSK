# Changelog

All notable changes to GSSK are documented here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project adheres to [Semantic Versioning](https://semver.org/).

---

## [Unreleased]

### Fixed

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
