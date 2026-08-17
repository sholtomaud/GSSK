# GSSK — Ecological-Economics & Countercurrent Work Package

> Task specification derived from a review of kernel **v4.0.0** against
> Odum (1973), *"Energy, Ecology, and Economics"* (Ambio 2:6, 220–227).
> Goal: complete Odum's money↔energy **countercurrent** with an **endogenous
> price** (so inflation emerges from falling net energy), resolve the
> `coupled_edge` / exchange-node duality, fix a build-portability regression,
> add an MCP interaction layer, and harden the archival/longevity story.

---

## Current state (what already exists)

The energy/emergy half of Odum's formalism is largely in place:

- Four-position inter-block channel is honoured (`carrier`=Code, `logic`+Q=Force+Flow, `quality_input`=Transformity).
- Multi-carrier support (Phase 5): `money` (AUD, conserved) and `goods` can be declared as independent conserved stocks.
- A working transaction diamond exists as a **node**: `NODE_EXCHANGE` (`src/gssk.c:47`), computed in `compute_exchange_node` (`src/gssk.c:~478`). It couples a forward goods flow to a backward money flow at a **fixed** ratio $F_{money} = \text{price} \cdot F_{goods}$, debiting the buyer stock and crediting inventory. Verified: running `tests/fuzz_corpus/seed_exchange_node.json` conserves money (buyer loss = sink gain) and holds $\text{spent}/\text{inventory} = \text{price}$ exactly.

The economic/countercurrent half has four gaps this package addresses:

1. **`price` is exogenous** — a constant scalar (`node_price`, `src/gssk.c:64`). Odum's inflation claim requires it to be endogenous.
2. **Money is a one-way drain**, not a conserved GNP loop (Odum Fig. 3).
3. **`coupled_edge` is declared but inert** — parsed to `coupled_idx` (`src/gssk.c:~2781–2786`) but never consumed in the derivative or quality pass.
4. **The countercurrent lives only in a fuzz seed** — no `examples/` entry, expected CSV, docs, or regression test.

Plus two non-model issues found during review:

5. **Build regression**: pinned `-Werror -std=c99` fails on modern GCC at `src/gssk.c:3240` and `:3247` (`-Werror=stringop-truncation`, motif `strncpy`).
6. **LLM interaction surface**: the CLI is batch-only; the kernel's stateful/introspective API is not exposed to tool-callers.

> Naming note: TODO.md Phase 0.1 renames `GSSK_* → GSK_*`. Symbol references
> below use current `GSSK_*` names; update in lockstep with that rename.

---

## Priority ordering

1. **Phase C** — Endogenous price (the actual Odum claim). ← *primary*
2. **Phase B** — Unify the transaction diamond (prerequisite plumbing for C).
3. **Phase D** — Money as a conserved GNP loop.
4. **Phase E** — First-class example + tests + docs.
5. **Phase A** — Build hygiene (small, do early to unblock CI).
6. **Phase F** — MCP interaction layer.
7. **Phase G** — Longevity / archival substrate.

---

## Phase A — Build & portability hygiene

### A.1 Fix `stringop-truncation` without weakening `-Werror`
- [ ] Replace the two truncating `strncpy` calls (`src/gssk.c:3240`, `:3247`) with a bounded, always-NUL-terminating copy. Prefer `snprintf(dst, sizeof dst, "%s", src)` or `memcpy` of `min(len, size-1)` followed by explicit `dst[size-1] = '\0'`.
- [ ] Audit remaining `strncpy` sites in `src/gssk.c` (mutation-record `target_id`/`payload`/`cause`, carrier `id`/`unit`, motif `canon`/`node_types`) for the same pattern.
- **Acceptance**: `make` (default `-Werror -std=c99`) builds clean on GCC ≥ 13 **and** Clang, no relaxed flags.

### A.2 Portable release flags
- [ ] Split release build: keep `-O3` but move `-march=native` behind an opt-in (`make RELEASE=1 NATIVE=1`). Distributed native binaries and the WASM build must be architecture-neutral (see Phase G).
- **Acceptance**: default release artifact runs on a different micro-arch than it was built on.

### A.3 CI compiler matrix
- [ ] Add GCC + Clang × `-std=c99` to `.github/workflows/deploy.yml` build gate so A.1 can't regress.

---

## Phase B — Unify the transaction diamond (`coupled_edge` ⟷ exchange node)

**Design decision (recommended).** Odum's small diamond is *both* a junction and
a coupling ratio. Rather than choose node-vs-edge, keep **two authoring
affordances backed by one physics primitive**:

- **Exchange node** (`NODE_EXCHANGE`): the full transaction *hub* — supports >1 in/out, optional dissipation ("energy cost of doing business" → heat sink, per Odum's reference notes), and buffering.
- **`coupled_edge`**: the *inline* form — a lightweight counterflow on an existing edge pair, no dedicated node.

Both must call a single shared helper so their behaviour cannot diverge, and
both must accept the **same `price` primitive** (constant *or* node reference,
see Phase C). Deprecate neither.

### B.1 Extract a shared coupling helper
- [ ] Factor the money/goods coupling out of `compute_exchange_node` into `apply_transaction_coupling(inst, F_primary, price, money_edge_idx, deriv)`: applies $F_{money} = \text{price} \cdot F_{primary}$ to the paired counter-flow and updates both endpoints' derivatives.
- [ ] Re-implement `compute_exchange_node` in terms of the helper (no behaviour change; the fuzz-seed output must be bit-identical).
- **Acceptance**: `./bin/gssk tests/fuzz_corpus/seed_exchange_node.json` output unchanged vs. baseline.

### B.2 Make `coupled_edge` live (edge-native path)
- [ ] In `compute_derivatives`, when an edge declares `coupled_edge` and a `price`: compute the primary flow from its own `logic`, then call `apply_transaction_coupling` to drive the paired edge in its declared (counter) direction.
- [ ] Guard against double-counting: an edge that is the *money* leg of a coupling must not also contribute its own independent `logic` flow.
- [ ] Extend `EdgeParams` in `gssk.schema.json` with an optional `price` (number **or** node-ref, mirroring Phase C), documented as required when `coupled_edge` is set.
- **Acceptance**: a two-edge model (goods edge + coupled money edge, no exchange node) reproduces the same trajectory as the equivalent exchange-node model to solver tolerance.

### B.3 Quality/emergy directionality across the coupling
- [ ] Ensure the quality-accounting pass treats the money leg as a counter-current (money carries no transformity into the structure; goods/energy do). Use `coupled_idx` to mark the money leg so it is excluded from emergy inflow to the target.
- **Acceptance**: `GSSK_GetQualityFlow` on the buyer/structure node reflects goods/energy emergy only, not money.

### B.4 Documentation of the two forms
- [ ] `docs/concepts.md`: one section, "The transaction diamond", showing node form vs. inline `coupled_edge` form and when to use each; note the optional dissipation term on the node form.

---

## Phase C — Endogenous price (Odum's core claim) — **PRIORITY**

Odum's causal chain: net energy $= $ gross $-$ energy spent getting energy
(pt. 1); as the feedback fraction rises, net energy per gross falls (pt. 2);
money circulates counter to energy, so if circulating money $M$ holds while
real work delivered $W$ falls, price $P = M/W$ (money per unit real work)
**rises** — emergent inflation (pt. 3). The task is to let $P$ be a state
driven by $M/W$, with $W$ tied to *net* energy from a depleting-resource
feedback loop.

### C.0 `price` as a constant-or-node reference (shared primitive)
- [ ] Add optional `price_node` to the exchange-node params and to the `coupled_edge` price field.
- [ ] In `apply_transaction_coupling`: `price = (price_idx >= 0) ? state[price_idx] : node_price;`.
- **Acceptance**: an exchange model with `price_node` pointing at a `constant` node reproduces the fixed-price result; changing that node's value changes the money flow.

### C.1 A ratio/division primitive
The current logic set (`constant`, `linear`, `interaction`, `limit`,
`threshold`) has no division, so $M/W$ is not yet expressible.
- [ ] Decide mechanism: **(a)** new edge logic `ratio` with $F = k \cdot Q_{origin} / (Q_{control} + \varepsilon)$, or **(b)** extend the existing `gain` node. *(Sub-task: confirm current `compute_gain_node` semantics before choosing.)*
- [ ] Implement with a guarded denominator ($\varepsilon$ floor) to avoid blow-up as $W \to 0$.
- [ ] Add IDC/RK4 treatment consistent with other primitives (linearise around current $Q$ for the incipient path).
- **Acceptance**: a unit model computing $P = M/W$ matches a hand-calculated ratio each step.

### C.2 A "delivered work" ($W$) signal
$W$ must be readable inside the derivative pass. Two options — pick one:
- [ ] **Lagged**: reuse the previous step's `GSSK_GetQualityFlow` / net-energy-flow into the structure (already computed post-step). Simple, explicit, stable for slow price dynamics.
- [ ] **Explicit state**: add a low-pass "delivered work" storage node fed by the goods/energy inflow to the structure, giving synchronous $W$.
- **Acceptance**: $W$ tracks the goods/energy inflow to the structure and falls when that inflow falls.

### C.3 Price dynamics
- [ ] Represent `price` as a `storage` node relaxing toward its target: $\dfrac{dP}{dt} = \alpha\left(\dfrac{M}{W} - P\right)$, with $\alpha$ a settable price-adjustment rate (a time constant, not an instantaneous reset — more stable and more Odum-like).
- [ ] Wire $M$ (circulating money / GNP proxy storage) and $W$ (C.2) into the ratio primitive (C.1) feeding $P$.
- **Acceptance**: with $M$ constant and $W$ falling, $P$ rises monotonically.

### C.4 Net-energy feedback loop (ties inflation to *falling net energy*)
- [ ] Build the Figure-2 structure into the reference model: getting energy costs energy fed back from the structure (a work-gate / interaction on the extraction flow), and the fuel resource is a depleting `storage`.
- [ ] As fuel depletes, the feedback fraction rises → net energy $W$ falls → $P$ rises. This is the piece that makes inflation *emerge from net-energy decline* rather than from supply/demand alone.
- **Acceptance (headline)**: a single run reproduces Odum's Fig. 1 boom/bust in the fuel/structure stocks **and** shows a monotonically rising price index over the depletion phase, with net-energy-per-gross falling in step. Captured as `examples/odum_countercurrent.json` (Phase E).

> Tiering guidance. **Tier 1** (fast, no new kernel logic): price as a storage
> driven by supply/demand ($M$ vs. inventory), routed through the exchange node
> — proves *emergent* price and can ship before C.1. **Tier 2** (C.1–C.4):
> price $= M/W$ with $W$ = net energy from the depletion loop — Odum's actual
> claim. Land Tier 1 first as a regression anchor, then Tier 2.

---

## Phase D — Money as a conserved GNP loop

Odum's Fig. 3 GNP is a *cycle*: money loops from the structure back to the
sources, counter-current to energy the whole way. The current seed drains money
to a sink.

- [ ] Author a model where money is a **closed conserved loop** (`spent → … → buyer`), not `→ sink`.
- [ ] Add a per-carrier conservation assertion using `GSSK_GetCarrierConservationError("money")`: for a closed money loop with no money source/sink, total money-Q must stay within tolerance.
- **Acceptance**: over a full run, $\sum Q_{money}$ is conserved to $\le$ `solver_tolerance`; energy is *not* conserved (dissipates to heat sink) — the asymmetry Odum draws.

---

## Phase E — First-class example, tests, docs

Promote the countercurrent from fuzz seed to a supported, regression-tested feature.

- [ ] `examples/odum_countercurrent.json` — the Phase C.4 model (carriers `energy`+`money`, exchange node/diamond, closed money loop, depletion feedback, endogenous price). Include an annotated variant like `household_model_annotated.json`.
- [ ] `tests/expected/odum_countercurrent.csv` — golden output; wire into the `make test` CSV-compare harness (`tests/csv_compare.c`).
- [ ] Keep `seed_exchange_node.json` in the fuzz corpus; add a closed-loop seed too.
- [ ] `docs/emergy_synthesis.md` or new `docs/countercurrent.md` — narrative tying the model to Odum (1973) points 1–3, with the Fig. 2 / Fig. 3 correspondence and the price-index plot.
- [ ] `docs/cookbook.md` — "Modelling money as a countercurrent" recipe.
- **Acceptance**: `make test` includes the countercurrent golden test; docs build under VitePress with the new page linked.

---

## Phase F — MCP interaction layer

The CLI (`gssk <model.json> [output.csv] [--report generativity]`) is batch-only
and hides the stateful API. Expose the interactive surface to LLM tool-callers
**without** writing an MCP transport in C.

### F.1 Decide ownership
- [ ] Confirm the split: CLI stays batch/repro; **MCP owns interactivity** (stepping, introspection, mutation). Record in `docs/`.

### F.2 Wrap an existing binding
- [ ] Build the MCP server over `python/gssk.py` **or** `js/gssk.js` (WASM) rather than the C core.
- [ ] Expose tools mapping the kernel API: `load_model`, `step(n)`, `get_state`, `get_node`, `add_node`, `add_edge`, `set_edge_k`, `deactivate_edge`, `get_quality_flow`, `serialize_snapshot`, `replay`, `get_motifs`/`get_generativity`.
- [ ] Return states as ID-keyed maps (not bare column indices) so callers survive topology mutation.
- **Acceptance**: an LLM can run a closed loop — load → step → inspect → mutate `k` → step → observe the effect — entirely through MCP tools.

### F.3 Binding parity check
- [ ] Verify the chosen binding actually surfaces the stateful calls (`GSSK_Step`, mutation, quality accessors). File follow-up tasks for any gaps before building tools on top.

---

## Phase G — Longevity / archival substrate

WASM is a sound *runtime* hedge (standardised, versioned, multi-implementation,
architecture-neutral — good against chipset churn) but is **not** the archival
artifact. The 100-year-reconstructible substrate is the portable C99 core + the
versioned JSON schema + prose docs. Make that explicit and protected.

- [ ] `docs/RECONSTRUCTION.md` — how to rebuild the kernel from source + schema + method docs alone, assuming no current runtime survives. Treat as a first-class deliverable.
- [ ] Enforce zero non-vendored deps in the C99 core (cJSON already vendored); add a CI check that the core compiles with only a C compiler + libm.
- [ ] Guarantee the JSON model format is self-describing: `schema_version` required (already true in v3), and every example carries it. Pin an archival copy of each schema version under `docs/`.
- [ ] Ensure distributed artifacts are architecture-neutral (see A.2); document that `-march=native` is dev-only.
- **Acceptance**: a reviewer can follow `RECONSTRUCTION.md` to rebuild `bin/gssk` from source with no network and no non-standard tooling.

---

## Open decisions (resolve before coding the affected phase)

- [ ] **B**: confirm "two authoring forms, one shared helper" over "desugar `coupled_edge` into a hidden node" (rejected here because a synthesized node leaks `__ex_*` columns into state/CSV and complicates round-trip/mutation).
- [ ] **C.1**: `ratio` edge logic vs. extending the `gain` node.
- [ ] **C.2**: lagged $W$ vs. explicit "delivered work" storage node.
- [ ] **C.3**: relaxation dynamics $dP/dt = \alpha(M/W - P)$ vs. algebraic $P = M/W$ each step.
- [ ] **F.2**: Python binding vs. WASM/JS binding as the MCP host.

---

## Definition of done (whole package)

- `make` builds clean under GCC and Clang with default `-Werror -std=c99`.
- `examples/odum_countercurrent.json` runs via the CLI and is covered by a golden CSV test in `make test`.
- Running it shows: fuel/structure boom-bust (Odum Fig. 1) **and** a rising price index driven by falling net energy (Odum pts. 1–3), with money conserved in a closed loop and energy dissipating to heat.
- `coupled_edge` and the exchange node produce identical physics via one shared helper; `price` accepts a constant or a node reference for both.
- An MCP server exposes the stateful API and supports a load→step→mutate→observe loop.
- `docs/` explains the countercurrent, the transaction diamond, and the reconstruction/longevity story.