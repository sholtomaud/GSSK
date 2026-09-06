# AGENTS.md: AI Developer Guidelines for GSSK

Welcome, fellow agent. This document provides the necessary context and standards for contributing to the General Systems Simulation Kernel (GSSK).

## 🎯 Project Overview
GSSK is a high-performance numerical engine for General Systems Theory and Odum Energy Systems Language. It simulates systems as coupled Ordinary Differential Equations (ODEs) using Euler or RK4 integration.

## 🛠 Tech Stack
- **Language**: C99 (Strict compliance: `-std=c99 -Wall -Wextra -Werror`).
- **JSON Parser**: `cJSON` (embedded in `src/cJSON.c`).
- **Build System**: GNU Make.
- **IDE Support**: `.clangd`, `compile_flags.txt`, and `compile_commands.json` are maintained in the root.

## 🏗 Architecture
- **Public API**: `include/gssk.h`. DO NOT put implementation details here.
- **Core Logic**: `src/gssk.c`.
- **Instance-Based**: All state is stored in an opaque `GSSK_Instance`. No global or static variables.
- **Memory Management**: Every `GSSK_Init` must have a corresponding `GSSK_Free`.

## 🧪 Testing & Verification
We use a **Registration-based Regression Testing** system.
- **Run Tests**: `make test`
- **Add New Test**: Create a JSON model in `examples/`, then run `make test-update`.
- **Comparison**: We use `bin/csv_compare` with a $10^{-6}$ tolerance to verify numerical stability.

## 📜 Coding Standards
1. **Absolute Paths**: When using IDE tools, prefer absolute paths for configuration (see `.clangd`).
2. **Fail-Safe Policy**:
   - Always check for `NaN` or `Inf` after a step.
   - Enforce physical conservation (clamping $Q < 0$ to $0.0$).
   - JSON parsing must be strict. Return `NULL` if the schema is violated.
3. **No Placeholders**: Never use placeholder code. If a feature isn't implemented, return a proper error code or use `(void)` for intentional stubs.

## 🚀 Deployment (WASM)
GSSK is designed for web integration. Ensure any core changes are compatible with `emcc` (Emscripten).
Command: `make wasm`

## 🎨 Web surfaces and the design tokens

Two web surfaces ship from this repository, and they must look like one site:
the VitePress docs at `/GSSK/` and the WASM demo at `/GSSK/demo/`. Both take
their colours from **`web/energese.css`** — one copy in this repository, imported
by `docs/.vitepress/theme/custom.css` and linked by `web/index.html`.

`web/energese.css` is itself a **duplicate** of the canonical file in the
`energese-project.github.io` repository. That is deliberate, and its header says
why: the demo has no build step, so consuming a published stylesheet would mean
adding npm to a C99 kernel whose releases are archived and citable. The price is
that drift is possible, so:

- **Never patch `web/energese.css` in place.** Edit the canonical copy, bump
  `--e-tokens-version`, and copy the whole file across. A one-sided edit is how
  the two repositories start disagreeing about the brand.
- Compare `--e-tokens-version` against the canonical file before assuming the
  palettes still agree.

### The series palette is a safety mechanism, not a preference

`--e-series-1` … `--e-series-8` and **the order they are in** were chosen by
running candidate orderings through a colour-blindness validator and keeping only
those that clear every adjacent-pair gate in both light and dark, against these
surfaces. Reordering them, substituting a hue, or adding a ninth silently breaks
that guarantee — the chart still renders, and two series become
indistinguishable to a protanope.

Past eight series, hue is reused and **line style** carries identity
(`SERIES_DASHES` in `web/index.html`). This replaced an `hsl(i * 137.5, ...)`
generator that produced unbounded unvalidated colours. Do not reintroduce a
generated palette.

A legend is always present on the demo chart. Three of the light-mode hues sit
below 3:1 against the white card, so identity must not rest on colour alone;
the legend's text labels are what discharge that.

### Verifying a change to either surface

Neither surface has an automated visual test, so look at it:

```sh
npm install && npm run docs:build     # the docs must build; the theme @import must resolve
npm run docs:preview                  # then open it in both colour schemes
```

For the demo, serve `web/` alongside a built `dist/gssk.js`. The deploy workflow
has a step that asserts every local asset `web/index.html` references reached the
Pages artifact — that catches a forgotten `cp`, which is otherwise invisible
until someone loads the deployed page.

## ✅ Programmatic Checks
Before submitting any changes, you MUST ensure:
1. `make clean && make` completes without any errors or warnings.
2. `make test` passes all regression tests with "PASSED" status.
3. `make test-advanced` and `make test-price-node` pass.
4. `make wasm-container` builds `dist/gssk.js` and `dist/gssk.wasm`. This runs
   Emscripten in a Linux container, so it works without a local `emcc` — there
   is no longer an "if available" excuse for shipping unverified WASM.
5. `make test-linux` builds and tests under **real GCC**. `/usr/bin/gcc` on macOS
   is a symlink to Apple clang, so a clean local build is NOT evidence of a clean
   CI build; GCC emits diagnostics clang does not and `CFLAGS` carries `-Werror`.

`make ci-local` runs 4 and 5 together.

`make demo` is also containerised, for a different reason: it plots with
matplotlib, which a bare system `python3` does not have. `Containerfile.demo`
carries a uv-managed interpreter and a pinned matplotlib
(`python/requirements-demo.txt`) alongside the C toolchain, so `make demo`
needs nothing installed on the host but the `container` CLI. It cleans first
and leaves Linux artefacts in `lib/`, so run `make clean && make all`
afterwards. `make demo-native` is the same demo without the container, for a
host that does have matplotlib.

## 🔀 Contribution Workflow

**Never commit directly to `main`.** Branch, push, and open a PR.

1. **Branch** — `feat/<task-slug>`, `fix/<slug>`, or `docs/<slug>`.
2. **Commit** — explain *why*, not just what. If you found a defect while doing
   something else, say so in the message; that is often the most valuable part.
3. **Push AND open a PR.** `gh pr create`. A pushed branch with no PR is not
   finished work: CI runs on `pull_request`, so a bare push produces **no test
   signal at all** and there is nothing for a reviewer to merge on.
4. **Wait for CI to pass.** All checks green — including `WASM build
   (emscripten)`, which exists specifically because export-list breakage used to
   reach `main` unverified.
5. **Do not merge your own PR.** Merging is the maintainer's decision. Hand over
   a green PR, not a branch.

### Concurrent branches must not edit the same line

**Git cannot merge two different insertions at the same point.** Not "merges them badly" —
cannot. Two branches cut from one commit that both append to the same list, heading, or
one-line collection will conflict pairwise, and the conflict is textual rather than
semantic: every resolution is "keep both", by hand, once per branch, after every merge.

This has happened here. Five branches were opened from one commit and each inserted at the
same three anchors — the `.PHONY` line in `Makefile`, a step after the same step in
`.github/workflows/deploy.yml`, and a section under `## [Unreleased]` in `docs/CHANGELOG.md`.
Merging the first broke the other four at once, and the last branch in the queue absorbed a
conflict from every merge ahead of it: five hand-resolutions to land work that never
disagreed about anything.

Before opening a PR, ask what *else* in flight touches the same line. The shared anchors here:

| Anchor | Do this instead |
| --- | --- |
| the `.PHONY:` list in `Makefile` | declare `.PHONY: <your-target>` on its own line beside your own rule — GNU make accumulates `.PHONY` prerequisites across declarations, and the file already does this for the `doco` targets |
| a target block above `# Schema conformance` | put it beside the suite it relates to, not at a shared landmark |
| a step after the same step in `deploy.yml` | anchor it after the step your work actually relates to |
| `## [Unreleased]` in `docs/CHANGELOG.md` | unavoidable — see below |

**The changelog cannot be anchored away**, because every entry legitimately belongs under the
same heading. So either land one PR before opening the next when both add entries, or accept
one re-resolve per merge and plan for it. Do not open five and hope.

**Resolving these is where content gets silently lost.** It happened in #55: the per-edge-flow
entries were in the feature commit and absent from the merge commit, so `main` shipped a
documented feature with no changelog record. Nothing failed; it took a bullet-level diff of the
merge parents to notice, and a follow-up PR to restore. So check the result against both inputs
rather than eyeballing it:

```sh
git show :2:docs/CHANGELOG.md   # ours
git show :3:docs/CHANGELOG.md   # theirs
# then diff the entry titles in each against the resolved file:
# anything missing was dropped, anything new was invented.
```

Fold entries into one heading per type while you are there — keeping both sides verbatim leaves
two `### Added` blocks under one version, which reads as though the release happened twice.

Once several branches are in flight and have each been consolidated differently, patching
individual hunks stops working: git reports interleaved hunks whose boundaries cut through
sections. Rebuild the whole block from the two stage entries instead. It is order-independent
and checkable.

### Reporting
State what you verified and what you did not. If a check was skipped or a test
failed, say so plainly with the output. "Should work" is not a result. Where you
made a judgement call under ambiguity, name the assumption in the PR body.

### Finishing a task
Work is not done when the code is written. It is done when it is **merged**.
A task marked complete whose code sits on an unmerged branch is a silent
blocker for everything downstream of it — this has already happened once here
(`c0-price-constant-or-node-ref`, whose stranded branch blocked Phase C until
it was found). Before marking a task done, confirm its code is on `main`.

## 📂 Directory Structure
- `include/`: Public headers.
- `src/`: Implementation.
- `bin/`: Built binaries (CLI, Compare tool).
- `lib/`: Compiled libraries.
- `tests/expected/`: "Gold Standard" results for regression.
- `examples/`: Reference JSON models.
- `web/`: The WASM demo (`index.html`) and the shared design tokens (`energese.css`).
- `docs/.vitepress/theme/`: VitePress restyled through CSS variables only.

## UPDATES PRE-COMMIT CHECK

- update the TODO.md to check off any completed items ythat are complete and tested.