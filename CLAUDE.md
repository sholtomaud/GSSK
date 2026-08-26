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

## Git workflow: always a worktree, never bare `main`

**Never commit to `main` directly, and never work in the primary checkout.** Every task gets its own
git worktree and branch, and lands through a pull request.

```sh
crux task worktree <slug>          # creates ../GSSK-<slug> on branch feat/<slug>
cd ../GSSK-<slug>
# ... red -> green -> refactor, in here ...
make test && make test-advanced && make test-schema
make ci-local                      # real GCC + Linux clang + WASM; see above
git push -u origin feat/<slug>
gh pr create --fill
```

`crux task worktree <slug>` is the entry point — it branches from `main`, reports whether local `main`
is ahead of or behind `origin`, and names the branch after the task, so the branch, the worktree
directory, and the crux task all carry the same slug.

Why a worktree rather than `git checkout -b`: the container mounts the checkout directory, and each
worktree gets its own `lib/`, `bin/` and `dist/`. Switching branches inside one checkout invalidates
that state and forces a rebuild, and a long `make test` run in one branch will read object files
built from another. Separate directories keep concurrent tasks genuinely independent — and because
`make ci-local` and `make wasm-container` leave Linux artefacts in `lib/`, a stray container build
cannot poison a checkout that another task is mid-test in.

If work is already sitting uncommitted in the primary checkout, move it rather than committing it
where it is — `git stash` is shared across worktrees:

```sh
git stash push -u -m "<slug>"      # in the primary checkout
crux task worktree <slug>
git -C ../GSSK-<slug> stash pop
```

Work that is not a crux task — a workflow fix, a docs correction — uses `git worktree add` directly
with a `chore/` branch, so the crux board keeps tracking deliverables rather than incidental edits:

```sh
git worktree add -b chore/<what> ../GSSK-<what> main
```

### Cleaning up after a merge

**Merging is not the end of the task; the worktree outlives the PR and has to be removed
deliberately.** A stale worktree keeps a branch checked out, holds its own built `lib/` and `bin/`,
and will quietly serve old objects to the next `make test` run. Once the PR is merged:

```sh
# From the primary checkout, which is always on main because nothing is worked on there.
git fetch origin --prune
git pull --ff-only origin main                         # so origin/main below includes the merge

git -C <worktree> status --short                       # must be empty
git -C <worktree> log --oneline origin/main..<branch>  # must be empty

git worktree remove ../GSSK-<slug>
git worktree prune
git branch -d <branch>                                 # refuses if unmerged, which is the point
git push origin --delete <branch>                      # GitHub does not always do this on merge
```

Run the two checks first and read them: `git worktree remove` refuses a dirty worktree, but a branch
whose commits never reached `origin/main` is exactly the case worth catching before anything is
deleted. Use `git branch -d`, never `-D` — the refusal is the safety check, so a `-D` that "just
works" means something was about to be lost. Deleting a merged branch is recoverable either way,
since its commits are reachable from the merge commit on `main`.

**`git branch -d` compares against local `main`, not `origin/main`.** This is why the
`git pull --ff-only` above comes first: skip it and `-d` refuses a perfectly merged branch, because
the merge commit is not in the local ref yet. Pull, then retry, before concluding anything is wrong.

There is one other refusal that is not what it looks like: a branch whose commits were **rebased or
re-created** before merging has a different SHA from what landed, so it is not an ancestor of `main`
even though its patch is. `git cherry origin/main <branch>` distinguishes the two — a `-` prefix
means already applied upstream, `+` means genuinely unique. Only `+` is a reason to stop. If every
commit is `-`, the content is safe and `-D` is justified; record the SHA first, and note that it
stays in the reflog for about 90 days regardless.

`git worktree list` should show only the primary checkout when no task is in flight; anything else is
leftover state.

## Before pushing

`make test`, `make test-advanced` and `make test-schema` must all pass with zero errors, and
`make ci-local` must build clean. Run them **in the worktree**, so what the PR claims is what was
actually tested.

`make ci-local` is not optional for anything touching the kernel, the export list or the build:
`/usr/bin/gcc` on macOS is Apple clang, so a green `make all` locally has not run real GCC and has
not built WASM at all. It leaves Linux artefacts behind — run `make clean && make all` afterwards
before trusting a native link.

## Project management

This repo is tracked in crux. Check `crux status` before starting work — it lists both the next
unblocked tasks and anything blocked. `crux ready` is a different thing despite the name: it is
release go/no-go (`Tasks done: N/M`, critical-path todos, a GO / NO-GO verdict), not a queue of what
to pick up next. Claim a task with `crux task start <slug>`.

### Go through crux, never through its database

crux keeps its state in a SQLite file at `~/.crux/crux.db`. **Never read or write it directly** — no
`sqlite3`, no Python `sqlite3` module, no SQL of any kind, not even a `SELECT` to "just check
something". Use one of the two supported entry points:

- **The `mcp__crux__*` MCP tools**, which are the preferred route in an agent session. The server is
  configured at user level, so it is available here without any project setup. The tools mirror the
  CLI: `crux_status`, `crux_ready`, `crux_task_show`, `crux_task_add`, `crux_task_update`,
  `crux_task_worktree`, `crux_dep_add`, `crux_cpm`, `crux_adr_add`, `crux_sync` and the rest. They
  may be deferred rather than listed up front — fetch a schema with
  `ToolSearch("select:mcp__crux__crux_task_add")` before calling.
- **The `crux` CLI** (`~/bin/crux`), which is equivalent and better when you want the formatted
  board: `crux status`, `crux ready`, `crux task show <slug>`, `crux task done <slug>`. Run
  `crux --help` for the current surface rather than guessing at flags — `crux task update --help` is
  parsed as a task slug, not as help.

The reason is not tidiness. A task row is not the whole of a task: adding or closing one also writes
an activity log entry, recomputes the critical path (`early_start`, `float_days`, `is_critical`), and
feeds estimate calibration from `actual_days`. A direct `INSERT` or `UPDATE` produces a row that
looks right and a board that is quietly wrong, and `crux sync` will then reconcile that wrongness
against GitHub. The database is also shared across every project on this machine, not just this repo,
so a malformed write is not contained here.

The two surfaces are close but not identical, and the CLI fails quietly rather than loudly. There is
an MCP `crux_task_update`, but no `crux task update` in the CLI — closing a task there is
`crux task done <slug> [--note "" --actual-days N]`. Invoking the MCP name against the CLI
(`crux task update <slug> --status done`) does not error: it reports `→ todo` and sets the status to
the opposite of what was asked. Read the line the command prints back and check it says what you
intended.

If something you need is genuinely not exposed by either entry point, say so and stop, rather than
reaching past them. That gap is worth reporting.



## Reference docs

- [docs/concepts.md](docs/concepts.md) — ESL semantics, node taxonomy, composites, integration
- [docs/adr/](docs/adr/) — architecture decisions; read before changing the transaction diamond or adding a logic primitive
- [gssk.schema.json](gssk.schema.json) — the **pre-expansion** model surface, not the post-expansion vocabulary
- [TODO.md](TODO.md) — phase roadmap; update it when completing items
