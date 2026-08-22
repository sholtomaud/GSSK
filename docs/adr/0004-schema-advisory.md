# ADR 0004 — The published schema is advisory, enforced in CI rather than at load time

- **Status**: accepted
- **Date**: 2026-08-19
- **Task**: `schema-v4-audit`
- **Supersedes**: nothing
- **Amended**: 2026-08-21 by `reject-unknown-node-types` — the open hazard in Consequences is closed; the decision itself stands unchanged.

## Context

`gssk.schema.json` ships in `dist/` and is one of the four artifacts consumers pin. GH #29 established that it had drifted badly from the parser — it rejected models the kernel accepts, and no serialised snapshot could validate against it at all. Fixing the content left the underlying question open: **is the schema a description the kernel enforces, or a contract consumers check for themselves?**

Nothing validated models against it at load time, and nothing checked the schema against the parser either. Both could drift again the moment someone adds a field.

## Decision

**The schema is advisory. It is not enforced inside `GSSK_Init`, and it is enforced in CI instead.**

Enforcing at load time would mean embedding a JSON Schema validator in the kernel. That is disproportionate and it contradicts what the kernel is: C99 with no dependencies beyond the standard library and a vendored JSON parser, small enough to audit and to compile to WebAssembly. A Draft 2020-12 validator is a substantial body of code — `$ref` resolution, `patternProperties`, `propertyNames`, `additionalProperties`, format assertions — and every line of it would ship in the WASM artifact for a check the consumer can perform once, before calling the kernel at all.

There is also a sequencing argument. The schema describes the **pre-expansion** surface: what a consumer writes. By the time `GSSK_Init` has finished, composites have expanded and the node set no longer matches the document the schema describes. Validating inside init would mean validating a document that the kernel is in the middle of transforming.

The kernel keeps doing what it already does: targeted structural checks with specific error messages (`Schema Error: Node 3 missing id/type/value`, `Linkage Error: Edge 2 unknown origin 'x'`). Those are better diagnostics than a schema path, because they name the semantic failure rather than the syntactic one.

## Consequences

**Drift is now caught by a test, not by hope.** `scripts/validate_models.py`, run by `make test-schema` and as part of `make test`, validates three corpora against the schema and fails the build if any member does not conform. CI installs `jsonschema` so the gate is real there; locally it skips with a message when the dependency is absent, so a bare checkout still builds. The check is not vacuous — deleting `config.rel_tol` from the schema makes it exit non-zero and name the offending path.

The three corpora are deliberately different in kind:

| Corpus | What a failure means |
| --- | --- |
| `examples/` | the schema rejects something a human would reasonably write |
| `tests/schema_fixtures/` | the schema stopped describing a corner no example reaches |
| `tests/results/serialized/` | the schema rejects the kernel's **own output** |

The third is the one that mattered. `examples/` alone only tests the schema against input, and the input side was the half that had already been fixed. `bin/dump_serialized` (`tests/dump_serialized.c`) loads every example and fixture, steps it, and writes both `GSSK_SerializeModel` and `GSSK_SerializeSnapshot` output for validation — so the format `dist/` consumers actually receive is now checked, which is what the Phase G archival story rests on.

**The audit found one real defect.** `config.rel_tol`, `abs_tol`, `h_min` and `h_max` are parsed by `GSSK_Init` and emitted by the serializer, but `Config` did not list them and set `additionalProperties: false`. Any model using DOPRI5 tolerances — including one the kernel had just written — failed validation against its own schema. All four are now described, and `tests/schema_fixtures/adaptive_config.json` keeps them covered. The rest of the surface checked out: node and edge fields, `NodeParams`, `EdgeParams`, `Carriers`, `ArchetypeDefn` (including `ports`), `Snapshot` and `MutationLog` all match what the parser reads. Two deliberate asymmetries are recorded rather than "fixed": `Node.type` cannot be a closed enum because archetype names are user-defined, and root-level `mutation_log` is archival — `GSSK_Init` restores a log only from `snapshot.mutation_log`.

**The fuzz corpus is deliberately exempt.** The task's acceptance criteria asked that `tests/fuzz_corpus/` validate too. That is the wrong requirement and it is not implemented. A fuzz corpus exists to carry malformed and degenerate input — one seed is not JSON at all, and `seed_empty.json` legitimately has no `nodes`. Requiring every seed to validate would either defeat the corpus's purpose or pressure someone into "fixing" seeds by making them well-formed, destroying the very cases fuzzing needs. Seed conformance is reported for visibility and gates nothing.

**The known hazard is now closed — by the parser, as this ADR anticipated.** When this decision was recorded, an unrecognised node `type` was not rejected: it fell back to `storage`, so a typo silently produced a different model rather than an error. This ADR named the right fix and declined to reach for the wrong one — *the parser should reject unknown types, not the kernel acquire a schema validator*. Task `reject-unknown-node-types` implements exactly that.

`GSSK_Init` now returns `GSSK_ERR_SCHEMA_VIOLATION` for any type that is neither one of the nine primitives, a built-in composite, nor an archetype declared in the model's own `archetypes` block, with a message naming the node id and the offending string. `GSSK_AddNode` rejects the same strings at runtime, and additionally refuses composite and archetype names, because it performs no expansion — a `producer` added at runtime would have become one storage node rather than its subgraph.

**This strengthens the decision rather than reversing it.** The reason a schema validator was the wrong instrument is exactly the reason the parser was the right one: `Node.type` cannot be a closed enum, because archetype names are user-defined and open-ended. A validator therefore cannot distinguish a typo from a legitimate archetype reference — but the parser can, because archetypes are parsed before nodes and it alone knows which names the model actually declared. Advisory validation stays advisory; the class of error it could never have caught is now caught where the information exists.

**Enforcement could still be added later without reversing this.** A `--validate` flag on the CLI, or an opt-in `GSSK_ValidateModel` in a separate translation unit that consumers link only if they want it, would both preserve the dependency-free core. Neither is needed now.

**The same closure, one level up: unknown *keys*.** `reject-unknown-node-types` closed the fallback for a node's `type`. Task `h8b-reject-unknown-model-keys` closes it for the keys themselves, at the root object, node objects, edge objects, edge `params` and `config`. A wrong key was never a smaller mistake than a wrong type — it is the same mistake one level up, and it had the same signature: silence.

The hazard is sharpest for a feature the kernel does not yet have. A model authored against a kernel that supports forcing functions, loaded by one that does not, used to return `GSSK_SUCCESS` and run to completion with its `forcing` block ignored. Its JSON — and therefore any external content hash of it, which is what `metadata.model_hash` carries, since the kernel round-trips that field and never computes it — says "forced". Its trajectory says "constant". Nothing reconciles the two. For a kernel whose case rests on reproducibility and on the Phase G archival story, that is the worst available failure mode: not a crash, a quietly different model.

**This does not weaken the advisory decision either; it is the same argument again.** The schema was *already* strict here — `additionalProperties: false` at the root and on `Node`, `Edge`, `EdgeParams` and `Config`, with `^_` annotation keys permitted by `patternProperties`. So the kernel was not made stricter than the published contract; it was brought into agreement with a contract the project had already published and was not honouring. `make test-schema` caught this for models in `examples/`, but nothing caught it at runtime for a *consumer's* model, which is where it matters. The accepted sets live next to the parser because that is where they can be kept true; each carries a comment saying it must be updated when a key is added, since a set that drifts from the parser reintroduces the bug in the opposite direction — rejecting valid models, which is worse, because it breaks working models rather than broken ones.

The `^_` exemption is load-bearing rather than a courtesy. `examples/household_model_annotated.json` and `examples/price_dynamics_model.json` carry `_note` and `_mechanism` blocks throughout. Annotations are how a model explains itself to a reader a decade later, which is most of what the archival substrate is for.

**And it found the defect this ADR's third corpus exists to find.** `build_topology_json` emits `"active": false` for an edge deactivated via `GSSK_DeactivateEdge` — a key that appears nowhere in the parser and that `Edge`'s `additionalProperties: false` already forbade. The kernel was emitting output its own schema rejects. It went unnoticed because no model in `examples/` or `tests/schema_fixtures/` has a deactivated edge, so `tests/results/serialized/` never contained one: the corpus was right, its coverage was not. `active` is now declared in the schema and accepted by the parser, and `tests/test_unknown_keys.c` covers the deactivated-edge round-trip directly, which is the only place that path is checked.

Round-trip *fidelity* for that flag is a separate matter and is deliberately not addressed here. Deactivation survives serialisation through `params.k`, which `GSSK_DeactivateEdge` sets to `0.0`, so the reloaded model reproduces the trajectory. What it does not reproduce is `edges[i].active` itself, which is read by topology classification — a reloaded edge is *active with k=0* rather than *inactive*. That is a behavioural change, not a key-validation one; it is filed separately rather than smuggled into this change.

---

## Amendment — 2026-08-23, `edge-active-flag-round-trip`: the deferred half, closed

The paragraph above deferred round-trip *fidelity* for `active` as a behavioural change rather than a key-validation one. It is now implemented, and the investigation it called for turned up a second, worse case and one inaccuracy in how the first was described here.

**`GSSK_Init` reads `active` and restores it, for edges and for nodes.** A `json_active()` helper in `src/gssk.c` returns the authored flag, defaulting to `true` when the key is absent. It is applied at every construction site: all three node paths in `GSSK_Init` (primitive, `system_frame` structural, and composite expansion), the edge parse, and both runtime parsers, `GSSK_AddNode` and `GSSK_AddEdge`.

**The flag is never inferred from `k`.** An edge whose author wrote `k: 0` is present in the network and carrying nothing; a deactivated edge has been taken out of it. Reading the flag back from the conductance would reproduce the trajectory and lose the topology — which is precisely the defect being fixed, restated one level down. `tests/schema_fixtures/deactivated_elements.json` pins both directions: an inactive edge with a *non-zero* `k`, and a live edge with `k: 0` and no `active` key.

**The node half was worse and was lost outright.** `GSSK_DeactivateNode` clears `nodes[i].active` and zeroes every edge touching the node, but `build_topology_json` emitted no `active` for nodes at all. An edge at least carried its deactivation accidentally, through `k`; a node has no `k` to hide behind, so node deactivation did not survive serialisation in any form. The serialiser now emits `"active": false` for an inactive node — only when false, matching the edge case — and `Node.active` is declared in `gssk.schema.json` and listed in `NODE_KEYS`.

Deactivating a node also deactivates its edges, and those edges carry their own flags in the serialised output. Restoring a node therefore does **not** re-cascade: each element is restored from what was written for it, which is the only rule that keeps a hand-authored model and a round-tripped one meaning the same thing.

**Correction to the paragraph above: `active` is not read by topology classification.** That claim does not survive checking. `GSSK_ReclassifyNetwork` sets `incipient_eligible = true` unconditionally — since Phase 1 gave every edge type an IDC treatment, it inspects no edges at all. The flag is read at roughly twenty other sites, and the ones that matter are those that **count elements rather than sum flows**, because those are exactly the ones `k = 0` cannot stand in for: `network_is_isolated_duet` requires exactly one active edge before the Riccati closed form is used, motif detection skips inactive nodes and edges, and `closed_system_conservation_error` sums only active nodes. The conclusion the original paragraph drew was right — a reloaded model could be treated differently from the model it was serialised from while producing identical numbers — but it named the wrong function. Restoration happens during node and edge parsing, which is before `GSSK_Init` calls `GSSK_ReclassifyNetwork`, so no additional call is needed either way.

**Why this needed its own suite.** `tests/test_unknown_keys.c` had covered the deactivated-edge round-trip since `h8b`, and it passed throughout, because it asserts the serialised output *reloads* — which it did, into a different model. A trajectory regression could not see this either: `k = 0` kills the flow whether the edge is active or not, so both instances produce the same CSV. `tests/test_deactivation_round_trip.c` therefore asserts on **motif count** and on **what happens when `k` is restored**: an edge that is genuinely inactive stays dead when its conductance comes back, and one that merely had `k = 0` starts flowing. Against the pre-fix kernel the suite fails eight assertions; the trajectory-equality case passes both before and after, which is the point — the numbers were never the problem.

**And the third corpus finally covers the path.** `tests/results/serialized/` never contained an `active` key, which is why the original defect survived; `tests/schema_fixtures/deactivated_elements.json` puts an inactive node and an inactive edge into it, so the schema is now checked against the kernel's own output for this case rather than only against models a human happened to write.

One loose end is recorded rather than fixed: `include/gssk.h` says an edge can be reactivated "by calling `GSSK_SetEdgeK()`". It cannot — `GSSK_SetEdgeK` writes `k` and does not touch `active`, and every flow site skips an inactive edge regardless of its conductance. There is no public reactivation call at all. That is an API gap, not a round-trip one, and widening the public surface belongs in its own change.
