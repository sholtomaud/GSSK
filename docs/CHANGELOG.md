# Changelog

All notable changes to GSSK are documented here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project adheres to [Semantic Versioning](https://semver.org/).

---

## [Unreleased]

### Added
- Phase 6 productionisation milestone

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
