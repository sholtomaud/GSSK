#!/usr/bin/env python3
"""Validate the bundled models against gssk.schema.json.

examples/ is the normative corpus: every model there is meant to be valid and
a failure is a real defect — either the model or the schema is wrong.

tests/fuzz_corpus/ is deliberately NOT held to that standard. A fuzz corpus
exists to carry malformed and degenerate input; requiring every seed to
validate would defeat its purpose and would pressure someone into "fixing"
seeds by making them well-formed. Seeds are reported for visibility only.

The kernel does not validate against this schema at load time (see
docs/adr/0004-schema-advisory.md), so this check is how the schema and the
parser are kept from drifting apart.

Exit codes: 0 pass or skipped, 1 an examples/ model failed validation.
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


def load(path):
    with open(path) as fh:
        return json.load(fh)


def main():
    schema = load(SCHEMA)
    # A malformed schema must fail loudly rather than silently accepting all.
    Draft202012Validator.check_schema(schema)
    validator = Draft202012Validator(schema)

    failures = 0

    normative = sorted(glob.glob("examples/*.json"))
    for path in normative:
        errors = sorted(validator.iter_errors(load(path)),
                        key=lambda e: list(e.absolute_path))
        if errors:
            failures += 1
            print(f"FAIL  {path}")
            for err in errors[:5]:
                loc = "/".join(str(p) for p in err.absolute_path) or "<root>"
                print(f"        {loc}: {err.message[:150]}")
    print(f"examples/: {len(normative) - failures}/{len(normative)} valid")

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
        print(f"\nFAILED: {failures} model(s) in examples/ do not match {SCHEMA}")
        return 1
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
