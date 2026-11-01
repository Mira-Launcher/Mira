#!/usr/bin/env bash
# Keeps the draft release of CMakeLists.txt's version in step with main: this commit as its target and
# out/'s packages as its files. Publishing the draft creates the tag; from then on builds leave it alone.
set -euo pipefail

version=$(sed -n 's/^project(.* VERSION \([0-9.]*\)).*/\1/p' CMakeLists.txt)
tag="v$version"
case $(gh release view "$tag" --json isDraft -q .isDraft 2>/dev/null || echo missing) in
  false)
    echo "$tag is already published"
    exit 0
    ;;
  missing) gh release create "$tag" --draft --target "$GITHUB_SHA" --title "Mira $version" --notes "" ;;
  *) gh release edit "$tag" --target "$GITHUB_SHA" ;;
esac
gh release upload "$tag" out/* --clobber
