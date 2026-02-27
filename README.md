# GSSK: General Systems Simulation Kernel

A high-performance numerical engine for General Systems Theory and Odum Energy Systems Language.

## 📁 Repository Structure

- `include/`: Public API headers (`gssk.h`).
- `src/`: Core implementation files.
- `bin/`: Compiled executables (CLI tool).
- `lib/`: Compiled libraries (static/shared).
- `tests/`: Numerical validation suite and benchmarks.
- `examples/`: Reference models and integration examples.
- `docs/`: VitePress documentation source.

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
- **[Live Demo](https://sholtomaud.github.io/GSSK/demo/)**: Interactive WASM simulation demo.
- **[Technical Documentation](https://sholtomaud.github.io/GSSK/)**: Comprehensive documentation powered by VitePress.

## 📦 NPM Installation (GitHub)
To use GSSK in your Node.js or browser project, install the pre-compiled distribution branch:
```bash
npm install sholtomaud/GSSK#dist
```
This ensures you receive the built WebAssembly binaries and TypeScript definitions without needing to compile the C source yourself.

## 🚀 Releasing
To create a new release:
1. Run `./scripts/release.sh [version]`
2. Push with tags: `git push origin main --tags`
The GitHub Action will automatically build the WASM, update the `dist` branch, and create a GitHub Release.
