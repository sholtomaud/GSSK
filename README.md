# GSSK: General Systems Simulation Kernel

[![DOI](https://zenodo.org/badge/DOI/10.5281/zenodo.22339313.svg)](https://doi.org/10.5281/zenodo.22339313)

A high-performance numerical engine for General Systems Theory and Odum Energy Systems Language.

## Quick Demo

```bash
make demo
```

Builds the CLI (if needed) and runs two models — a simple exponential decay and a 4-carrier household ecological-economy — printing the CSV header and first rows of each:

```
=== Decay model (exponential decay, RK4) ===
time,biomass,environment
0.0000,100.000000,0.000000
0.5000,97.530991,2.469009
1.0000,95.122942,4.877058
...

=== Household model (4-carrier ecological-economy) ===
time,salary,bank_account,super_fund,...
0.0000,1.000000,5000.000000,50000.000000,...
... (241 data rows, 24 columns)
```

The decay model follows Q(t) = 100·exp(−0.05·t). The household model has 23 state nodes across money, energy, material, and information carriers.

### Run all regression tests

```bash
make test
```

### Python binding demo

```bash
make demo-python
```

Prints model name, node/edge/carrier counts, final state, and an error check against the analytical solution.

### Benchmark

```bash
make bench
```

---

## Repository Structure

- `include/` — Public API headers (`gssk.h`)
- `src/` — Core C99 implementation
- `bin/` — Compiled executables (CLI tool)
- `lib/` — Compiled libraries (static/shared)
- `tests/` — Regression suite, fuzz target, fuzz corpus
- `examples/` — Reference JSON models
- `bench/` — Benchmark runner and generated models
- `python/` — Python ctypes binding
- `js/` — JavaScript/TypeScript WASM wrapper
- `docs/` — VitePress documentation source
- `scripts/` — Release tooling

## Building

### Prerequisites
- GCC or Clang
- Make
- Python 3 (for Python binding and bench-gen)

### Build

```bash
make            # native library + CLI
make shared     # shared library for Python binding
make wasm       # WebAssembly (requires emscripten)
```

## Testing

```bash
make test              # regression suite (CSV diff)
make test-python       # Python binding (31 tests)
swift test             # Swift binding (61 tests)
make test-asan         # AddressSanitizer + UBSan (requires clang)
make coverage-check    # lcov coverage gate ≥ 85%
make test-valgrind     # Valgrind leak check (Linux)
make fuzz-run          # 30 s LibFuzzer run (requires clang)
```

## Documentation

- **[Live Docs](https://sholtomaud.github.io/GSSK/)** — VitePress site (Concepts, API Reference, Cookbook, Examples)
- **[Interactive Demo](https://sholtomaud.github.io/GSSK/demo/)** — browser WASM simulation
- [docs/concepts.md](docs/concepts.md) — ESL, integration methods, carriers, sensitivity
- [docs/api-reference.md](docs/api-reference.md) — C / Python / JS / Swift API
- [docs/cookbook.md](docs/cookbook.md) — parametric sweep, sensitivity, snapshot round-trip
- [docs/CHANGELOG.md](docs/CHANGELOG.md) — release history

## NPM Installation (GitHub)

```bash
npm install sholtomaud/GSSK#dist
```

Installs the pre-compiled WASM binaries and TypeScript definitions.

## Releasing

```bash
./scripts/release.sh 3.6.0
git push origin main --tags
```

The script bumps `GSK_VERSION_*` in `include/gssk.h`, updates `docs/CHANGELOG.md`, commits and tags. The GitHub Action builds WASM, updates the `dist` branch, and creates a GitHub Release.

## Security

See [SECURITY.md](SECURITY.md) for vulnerability reporting and the threat model.

## License

MIT — see [LICENSE](LICENSE). Bundles [cJSON](https://github.com/DaveGamble/cJSON) (MIT).
