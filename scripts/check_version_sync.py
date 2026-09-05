#!/usr/bin/env python3
"""Fail when package.json and include/gssk.h disagree about the version.

The npm package sat at 1.0.0 through five majors of the C kernel. Nothing
noticed, because the two version numbers live in different files and only
scripts/release.sh ever touched one of them. A consumer who pins `gssk` from
npm gets `include/gssk.h` out of node_modules, so a stale package version
serves a header from a kernel that has not existed for years — which is
exactly how GIP-0001 came to be written against API comments that were four
majors out of date.

Three things are checked, all of them cheap and all of them things that have
actually gone wrong:

  1. package.json "version" == GSK_VERSION_STRING in include/gssk.h.
  2. GSK_VERSION_STRING == "<MAJOR>.<MINOR>.<PATCH>" from the numeric macros
     beside it, since a hand-edited header can desynchronise from itself.
  3. package.json "files" ships gssk.schema.json. The schema is the only
     machine-readable statement of the model vocabulary; downstream
     consumers that cannot read it hand-maintain a copy of the node-type
     enum instead, with nothing to detect drift.
  4. CITATION.cff "version" == GSK_VERSION_STRING. This is the file Zenodo
     reads when minting a DOI and GitHub reads for "Cite this repository",
     so a stale value does not just sit in the tree — it is published as
     the citation for the archived release and cannot be corrected in
     place afterwards. It said 5.1.0 while the archived release was v5.2.0,
     because nothing bumped it and nothing checked it.

Standard library only, on purpose: this gate has to run everywhere the build
runs, including a checkout with no pip packages installed.

Exit codes: 0 all three agree, 1 at least one disagrees.
"""
import json
import re
import sys

PACKAGE = "package.json"
HEADER = "include/gssk.h"
SCHEMA = "gssk.schema.json"
CITATION = "CITATION.cff"

MACROS = ("GSK_VERSION_MAJOR", "GSK_VERSION_MINOR", "GSK_VERSION_PATCH")


def read_header(path):
    """Return (major, minor, patch, version_string) as written in the header."""
    with open(path, encoding="utf-8") as handle:
        text = handle.read()

    numbers = []
    for macro in MACROS:
        match = re.search(rf"^#define\s+{macro}\s+(\d+)\s*$", text, re.MULTILINE)
        if match is None:
            raise LookupError(f"{path}: no #define {macro}")
        numbers.append(match.group(1))

    match = re.search(
        r'^#define\s+GSK_VERSION_STRING\s+"([^"]*)"\s*$', text, re.MULTILINE
    )
    if match is None:
        raise LookupError(f"{path}: no #define GSK_VERSION_STRING")

    return numbers[0], numbers[1], numbers[2], match.group(1)


def read_citation_version(path):
    """Return the version string in CITATION.cff.

    Parsed with a regex rather than a YAML library on purpose: this gate runs
    everywhere the build runs, including a checkout with no pip packages, and
    PyYAML is not in the standard library.
    """
    with open(path, encoding="utf-8") as handle:
        text = handle.read()

    match = re.search(r"""^version:\s*["']?([^"'\s]+)["']?\s*$""", text, re.MULTILINE)
    if match is None:
        raise LookupError(f"{path}: no top-level 'version:' key")
    return match.group(1)


def main():
    try:
        major, minor, patch, header_version = read_header(HEADER)
    except (OSError, LookupError) as exc:
        print(f"FAILED: {exc}")
        return 1

    try:
        with open(PACKAGE, encoding="utf-8") as handle:
            package = json.load(handle)
    except (OSError, ValueError) as exc:
        print(f"FAILED: {PACKAGE}: {exc}")
        return 1

    failures = []

    composed = f"{major}.{minor}.{patch}"
    if header_version != composed:
        failures.append(
            f"{HEADER}: GSK_VERSION_STRING is \"{header_version}\" but the "
            f"GSK_VERSION_MAJOR/MINOR/PATCH macros compose to \"{composed}\""
        )

    package_version = package.get("version")
    if package_version != header_version:
        failures.append(
            f"{PACKAGE}: \"version\" is {package_version!r} but "
            f"{HEADER} says GSK_VERSION_STRING is \"{header_version}\". "
            f"Bump {PACKAGE} — scripts/release.sh does this for you."
        )

    try:
        citation_version = read_citation_version(CITATION)
    except (OSError, LookupError) as exc:
        failures.append(str(exc))
    else:
        if citation_version != header_version:
            failures.append(
                f"{CITATION}: \"version\" is {citation_version!r} but "
                f"{HEADER} says GSK_VERSION_STRING is \"{header_version}\". "
                f"Zenodo publishes this as the citation for the archived "
                f"release, so it cannot be fixed after the fact. "
                f"Bump {CITATION} — scripts/release.sh does this for you."
            )

    files = package.get("files") or []
    if SCHEMA not in files:
        failures.append(
            f"{PACKAGE}: \"files\" does not ship {SCHEMA}, so consumers of "
            f"the npm package cannot read the model vocabulary. "
            f"Currently ships: {files}"
        )

    if failures:
        for failure in failures:
            print(f"FAILED: {failure}")
        return 1

    print(f"OK: {PACKAGE}, {HEADER} and {CITATION} agree on "
          f"{header_version}; {SCHEMA} is shipped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
