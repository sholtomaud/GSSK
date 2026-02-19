# GSSK: General Systems Simulation Kernel

A high-performance numerical engine for General Systems Theory and Odum Energy Systems Language.

## 📁 Repository Structure

- `include/`: Public API headers (`gssk.h`).
- `src/`: Core implementation files.
- `bin/`: Compiled executables (CLI tool).
- `lib/`: Compiled libraries (static/shared).
- `tests/`: Numerical validation suite and benchmarks.
- `examples/`: Reference models and integration examples.

## 🛠 Building

### Prerequisites
- GCC or Clang
- Make

### Build Command
```bash
make
```
This will generate `lib/libgssk.a` and `bin/gssk`.

### Running Tests
```bash
make test
```

### WASM Compilation
```bash
make wasm
```

## 📖 Documentation
- [REQUIREMENTS.md](REQUIREMENTS.md): User-facing needs and philosophy.
- [SPECIFICATION.md](SPECIFICATION.md): Technical blueprint and mathematical definitions.
