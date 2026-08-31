#!/usr/bin/env bash
# scripts/release.sh — Bump version, update header, tag release.
#
# Usage: ./scripts/release.sh <major.minor.patch>
# Example: ./scripts/release.sh 3.6.0

set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 <major.minor.patch>"
  exit 1
fi

VERSION="$1"
TAG="v$VERSION"

# Validate semver shape
if ! [[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "ERROR: Version must be in major.minor.patch format (e.g. 3.6.0)" >&2
  exit 1
fi

IFS='.' read -r MAJOR MINOR PATCH <<< "$VERSION"

HEADER="include/gssk.h"
CHANGELOG="docs/CHANGELOG.md"
PACKAGE="package.json"

# ── 1. Bump version in include/gssk.h ─────────────────────────────────────
if [[ ! -f "$HEADER" ]]; then
  echo "ERROR: $HEADER not found" >&2
  exit 1
fi

sed -i.bak \
  -e "s/^#define GSK_VERSION_MAJOR .*/#define GSK_VERSION_MAJOR $MAJOR/" \
  -e "s/^#define GSK_VERSION_MINOR .*/#define GSK_VERSION_MINOR $MINOR/" \
  -e "s/^#define GSK_VERSION_PATCH .*/#define GSK_VERSION_PATCH $PATCH/" \
  -e "s/^#define GSK_VERSION_STRING \".*\"/#define GSK_VERSION_STRING \"$VERSION\"/" \
  "$HEADER"
rm -f "${HEADER}.bak"
echo "Updated $HEADER → $VERSION"

# ── 2. Bump version in package.json ───────────────────────────────
# The npm version is not cosmetic: consumers pin it and then read
# include/gssk.h out of node_modules, so leaving it behind serves a header
# from a kernel that no longer exists. Skipping this step is how the package
# sat at 1.0.0 through five majors. scripts/check_version_sync.py is the gate
# that catches a miss; this is the step that keeps it green.
if [[ ! -f "$PACKAGE" ]]; then
  echo "ERROR: $PACKAGE not found" >&2
  exit 1
fi

sed -i.bak \
  -e "s/^\(  \"version\": \)\".*\",$/\1\"$VERSION\",/" \
  "$PACKAGE"
rm -f "${PACKAGE}.bak"

# Trust nothing: the sed above assumes a key position, so confirm the file is
# still valid JSON and actually carries the new version before committing it.
python3 -c "import json,sys; v=json.load(open(sys.argv[1]))['version']; sys.exit(0 if v==sys.argv[2] else f'ERROR: {sys.argv[1]} version is {v!r}, expected {sys.argv[2]!r}')" \
  "$PACKAGE" "$VERSION"
echo "Updated $PACKAGE → $VERSION"

# ── 3. Update [Unreleased] block in CHANGELOG ─────────────────────────────
if [[ -f "$CHANGELOG" ]]; then
  TODAY=$(date +%Y-%m-%d)
  # Replace first "## [Unreleased]" heading with versioned heading
  sed -i.bak \
    "s/^## \[Unreleased\]$/## [$VERSION] — $TODAY/" \
    "$CHANGELOG"
  # Insert fresh Unreleased section above it
  sed -i.bak \
    "/^## \[$VERSION\]/i\\
## [Unreleased]\\
\\
### Added\\
\\
---\\
" \
    "$CHANGELOG"
  rm -f "${CHANGELOG}.bak"
  echo "Updated $CHANGELOG"
fi

# ── 4. Stage and commit ────────────────────────────────────────────────────
git add "$HEADER" "$PACKAGE"
[[ -f "$CHANGELOG" ]] && git add "$CHANGELOG"
git commit -m "chore: release $TAG"
echo "Committed: chore: release $TAG"

# ── 5. Tag ────────────────────────────────────────────────────────────────
git tag -a "$TAG" -m "Release $TAG"
echo "Tagged: $TAG"

echo ""
echo "Done. Run the following to publish:"
echo "  git push origin main --tags"
