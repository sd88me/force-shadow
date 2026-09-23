#!/usr/bin/env bash
# Build the public release zip: dist/ForceShadow-<version>.zip, unpacking to
#   AddOns/ForceShadow/              core: visual + audio layers (required)
#   AddOns/ForceShadowTestTone/      optional: test-tone producer
#   AddOns/ForceAudioJackSkipback/   optional: Skipback recorder (folder name
#                                    kept because force-cratedigger references it)
# Uses the prebuilt binaries already in addon*/. Rebuild them first
# (README.md "Building") if the sources changed.
set -euo pipefail
cd "$(dirname "$0")/.."
VER="${1:-$(git describe --tags --always)}"
STAGE="$(mktemp -d)"; trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/AddOns" dist
cp -r addon          "$STAGE/AddOns/ForceShadow"
cp -r addon-testtone "$STAGE/AddOns/ForceShadowTestTone"
cp -r addon-skipback "$STAGE/AddOns/ForceAudioJackSkipback"
chmod 0755 "$STAGE"/AddOns/*/*.sh "$STAGE"/AddOns/*/*.so \
    "$STAGE/AddOns/ForceShadow/force_shadow_exitwatch" \
    "$STAGE/AddOns/ForceShadowTestTone/injectTone" \
    "$STAGE/AddOns/ForceAudioJackSkipback/skipbackHost"
rm -f "dist/ForceShadow-$VER.zip"
python3 -c "import shutil,sys; shutil.make_archive(sys.argv[1], 'zip', sys.argv[2], 'AddOns')" "dist/ForceShadow-$VER" "$STAGE"
python3 -m zipfile -l "dist/ForceShadow-$VER.zip"
