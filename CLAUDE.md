# CLAUDE.md

**[AGENTS.md](AGENTS.md) is the canonical standard for this repository. Read it first.**

Everything there applies to Claude Code: architecture, coding standards,
programmatic checks, and the contribution workflow. This file deliberately does
**not** restate those rules — two copies of a standard drift apart, and then
neither can be trusted. Add new standards to `AGENTS.md`, not here.

## The three that get broken most

Repeated here only because they have actually been violated in this repo, not
to duplicate the standard:

1. **Push *and* open a PR.** CI runs on `pull_request`. A pushed branch with no
   PR has been tested by nothing. Do not stop at the push.
2. **Do not merge your own PR.** Hand over a green PR and let the maintainer
   decide.
3. **A task is done when it is merged**, not when it is written. Check that the
   code is on `main` before marking it complete.

## Claude Code specifics

**Verify on Linux before claiming a clean build.** `/usr/bin/gcc` on macOS is
Apple clang, so `make all` locally has never once run real GCC. Use the
containerised toolchains:

```
make ci-local          # real GCC + Linux clang + WASM
make test-linux        # real GCC only (Ubuntu 24.04, matches CI)
make wasm-container    # Emscripten without a local emcc
```

These need the Apple `container` CLI. `make wasm-container` in particular
removes any excuse for an unverified export list.

**Task tracking is `crux`.** One global binary and one shared database, scoped
to this project by `.crux/project.json`. There is no project-local `crux`
executable. The database spans several projects, so filter by this project's id
when querying it directly.

**Do not trust "done" without checking.** Task status and merged code are
separate facts. Confirm against `main`.

## Reference docs

- [docs/concepts.md](docs/concepts.md) — ESL semantics, node taxonomy, composites, integration
- [docs/adr/](docs/adr/) — architecture decisions; read before changing the transaction diamond or adding a logic primitive
- [gssk.schema.json](gssk.schema.json) — the **pre-expansion** model surface, not the post-expansion vocabulary
- [TODO.md](TODO.md) — phase roadmap; update it when completing items
