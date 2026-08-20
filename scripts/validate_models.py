#!/usr/bin/env python3
"""Validate the bundled models against gssk.schema.json.

Three normative corpora, each of which must validate:

  examples/                — the models a human writes. A failure here means
                             the schema rejects hand-authored input.
  tests/schema_fixtures/   — models that exist only to exercise corners the
                             examples do not reach (adaptive-solver config,
                             archived mutation logs).
  tests/results/serialized/ — what GSSK_SerializeModel and
                             GSSK_SerializeSnapshot actually emit, written by
                             bin/dump_serialized. This is the half the schema
                             kept drifting from: it is the format the archival
                             story rests on, and nothing regenerates it by
                             hand. Absent when the validator is run without
                             building first, in which case it is skipped.

tests/fuzz_corpus/ is deliberately NOT held to that standard. A fuzz corpus
exists to carry malformed and degenerate input; requiring every seed to
validate would defeat its purpose and would pressure someone into "fixing"
seeds by making them well-formed. Seeds are reported for visibility only.

The kernel does not validate against this schema at load time (see
docs/adr/0004-schema-advisory.md), so this check is how the schema and the
parser are kept from drifting apart.

Exit codes: 0 pass or skipped, 1 a normative model failed validation.
"""
import glob
import json
import sys

try:
    from jsonschema import Draft202012Validator
except ImportError:
    print("SKIPPED: python3 -m pip install jsonschema  (not installed)")
    sys.exit(0)

SCHEMA = "gssk.schema.json"
SERIALIZED = "tests/results/serialized/"
NORMATIVE = ("examples/", "tests/schema_fixtures/", SERIALIZED)


def load(path):
    with open(path) as fh:
        return json.load(fh)


def main():
    schema = load(SCHEMA)
    # A malformed schema must fail loudly rather than silently accepting all.
    Draft202012Validator.check_schema(schema)
    validator = Draft202012Validator(schema)

    failures = 0

    for corpus in NORMATIVE:
        paths = sorted(glob.glob(f"{corpus}*.json"))
        if not paths:
            why = ("not generated — run 'make test-schema'"
                   if corpus == SERIALIZED else "empty")
            print(f"{corpus}: skipped ({why})")
            continue
        bad = 0
        for path in paths:
            errors = sorted(validator.iter_errors(load(path)),
                            key=lambda e: list(e.absolute_path))
            if errors:
                bad += 1
                print(f"FAIL  {path}")
                for err in errors[:5]:
                    loc = "/".join(str(p) for p in err.absolute_path) or "<root>"
                    print(f"        {loc}: {err.message[:150]}")
        failures += bad
        print(f"{corpus}: {len(paths) - bad}/{len(paths)} valid")

    seeds = sorted(glob.glob("tests/fuzz_corpus/*"))
    ok = invalid = unparseable = 0
    for path in seeds:
        try:
            doc = load(path)
        except Exception:
            unparseable += 1
            continue
        if list(validator.iter_errors(doc)):
            invalid += 1
        else:
            ok += 1
    if seeds:
        print(f"tests/fuzz_corpus/: {ok} valid, {invalid} invalid, "
              f"{unparseable} unparseable (informational — not a gate)")

    if failures:
        print(f"\nFAILED: {failures} model(s) do not match {SCHEMA}")
        return 1
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
