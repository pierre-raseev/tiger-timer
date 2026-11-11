#!/bin/bash
# Build the firmware and publish GitHub release vX.Y.Z (X.Y.Z = FW_VERSION in tiger_timer.ino)
# with TigerTimer-vX.Y.Z.bin attached - the file you upload from Remote Config > Firmware update.
# Run from anywhere, after committing and pushing the change. Usage: tools/release.sh "notes"
set -euo pipefail
cd "$(dirname "$0")/.."
VERSION=$(sed -n 's/^const char\* FW_VERSION = "\([0-9.]*\)";.*/\1/p' tiger_timer/tiger_timer.ino)
[ -n "$VERSION" ] || { echo "FW_VERSION not found"; exit 1; }
TAG="v$VERSION"
git rev-parse -q --verify "refs/tags/$TAG" >/dev/null && { echo "$TAG already exists - bump FW_VERSION"; exit 1; }
[ -z "$(git status --porcelain)" ] || { echo "Commit your changes first"; exit 1; }

python3 tools/embed_config_html.py
git diff --quiet tiger_timer/config_html.h || { echo "config_html.h was stale - commit the regenerated file first"; exit 1; }

BUILD=$(mktemp -d)
arduino-cli compile -b esp32:esp32:XIAO_ESP32C6:CDCOnBoot=cdc --build-path "$BUILD" tiger_timer
BIN="$BUILD/TigerTimer-$TAG.bin"
cp "$BUILD/tiger_timer.ino.bin" "$BIN"
python3 -c "import sys; d=open(sys.argv[1],'rb').read(); assert b'TigerTimer-firmware-id-7d1e9a' in d, 'marker missing'" "$BIN"

git tag -a "$TAG" -m "Tiger Timer $TAG"
git push -q origin "$TAG"
gh release create "$TAG" "$BIN" --title "Tiger Timer $TAG" --notes "${1:-Tiger Timer $TAG}

Install: Remote Config > Firmware update > choose TigerTimer-$TAG.bin > Install update, then press the arcade button on the box."
echo "Released $TAG"
