#!/bin/bash

# Simple script to handle version tagging
# Usage: ./scripts/release.sh [version]
# Example: ./scripts/release.sh 1.0.1

if [ -z "$1" ]; then
    echo "Usage: $0 [version]"
    echo "Current version in package.json: $(grep version package.json | head -1 | awk -F: '{ print $2 }' | sed 's/[", ]//g')"
    exit 1
fi

VERSION=$1
TAG="v$VERSION"

# Update package.json
sed -i "s/\"version\": \".*\"/\"version\": \"$VERSION\"/" package.json

# Commit and tag
git add package.json
git commit -m "chore: release $TAG"
git tag -a "$TAG" -m "Release $TAG"

echo "Version updated to $VERSION and tag $TAG created."
echo "Run 'git push origin main --tags' to trigger the release workflow."
