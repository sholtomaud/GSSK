# GSSK Testing Specification

## 1. Unit Testing Strategy
GSSK uses a lightweight, dependency-free testing harness written in C.

- **Location**: `tests/`
- **Runner**: `tests/test_runner.c`
- **Objectives**:
  - Verify JSON parsing robustness.
  - Validate mathematical accuracy of ODE logic.
  - Ensure zero memory leaks (via Valgrind/ASan).

## 2. Integration Tests (Reference Models)
Each model in `tests/models/` consists of:
1. `model.json`: Input topology.
2. `expected.csv`: Golden trajectory for comparison.

The test runner will:
1. Load `model.json`.
2. Run simulation for $N$ steps.
3. Compare result $Q(t)$ against `expected.csv` with a tolerance of $10^{-6}$.

## 3. Visual Regression (Optional)
For frontend verification, the `tests/visual/` directory (future) will contain scripts to compare plots generated from the CSV output against reference images.

## 4. Performance Benchmarks
A dedicated `tests/benchmark.c` will measure:
- Mean time per step.
- Startup latency.
- Memory footprint per 1,000 nodes.
