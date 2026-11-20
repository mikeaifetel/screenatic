#!/usr/bin/env bash
# Adds UxPlay (GPL-3.0) as a git submodule under protocol-airplay/third_party/UxPlay.
# Pin UXPLAY_REF to a release tag before shipping; check the tag list on GitHub.
set -euo pipefail

UXPLAY_URL="https://github.com/FDH2/UxPlay.git"
UXPLAY_REF="${UXPLAY_REF:-v1.73.7}"
DEST="protocol-airplay/third_party/UxPlay"

cd "$(dirname "$0")/.."

if [ ! -d .git ]; then
  echo "Run 'git init' in the project root first." >&2
  exit 1
fi

if [ ! -d "$DEST" ]; then
  git submodule add "$UXPLAY_URL" "$DEST"
fi
git -C "$DEST" fetch --tags
git -C "$DEST" checkout "$UXPLAY_REF"

echo "UxPlay checked out at $(git -C "$DEST" describe --tags --always)."
echo "Next: cross-compile OpenSSL (libcrypto) and libplist for armeabi-v7a/arm64-v8a"
echo "into protocol-airplay/third_party/prebuilt/<abi>/{include,lib} - see README."
