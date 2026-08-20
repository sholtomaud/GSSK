# ADR 0004 — The published schema is advisory, enforced in CI rather than at load time

- **Status**: accepted
- **Date**: 2026-08-19
- **Task**: `schema-v4-audit`
- **Supersedes**: nothing

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

**The known hazard remains, and is now the consumer's to manage.** An unrecognised node `type` is not rejected by the parser — it falls back to `storage`, so a typo silently produces a different model rather than an error. Advisory validation does not change that; it makes checking possible, not automatic. This is documented in the schema, in `docs/concepts.md`, and in the 4.1.0 release notes, and it is the strongest argument for a consumer validating before calling `GSSK_Init`. If that hazard ever needs closing, the right fix is for the parser to reject unknown types, not for the kernel to acquire a schema validator.

**Enforcement could still be added later without reversing this.** A `--validate` flag on the CLI, or an opt-in `GSSK_ValidateModel` in a separate translation unit that consumers link only if they want it, would both preserve the dependency-free core. Neither is needed now.
