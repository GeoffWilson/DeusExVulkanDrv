#!/usr/bin/env bash
# Fetch the parts of Khronos's OpenXR SDK the path tracer's helper uses for a
# headset, for the 64-bit Windows build, from Linux.
#   fetch-openxr.sh [OutputDir]
#
# The loader is taken from Khronos's own release at a pinned version rather
# than built here. Only what the helper needs is kept, in OutputDir (default
# ../.openxr, beside the .xwin folder):
#   include/openxr/     the headers
#   openxr_loader.dll   the 64-bit loader (static CRT), which ships beside the
#                       helper and finds whichever runtime is active -
#                       SteamVR's, Meta's - when a headset is asked for
#   LICENSE             the Apache 2.0 licence it comes under
set -euo pipefail

OPENXR_VERSION=1.1.63
ZIP="openxr_loader_windows-$OPENXR_VERSION.zip"
URL="https://github.com/KhronosGroup/OpenXR-SDK-Source/releases/download/release-$OPENXR_VERSION/$ZIP"

REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$REPO/../.openxr}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
curl -fsSL --retry 3 -o "$WORK/$ZIP" "$URL"
unzip -q -o "$WORK/$ZIP" 'include/openxr/*' 'x64/bin/openxr_loader.dll' 'share/doc/openxr/LICENSE' -d "$WORK/sdk"

rm -rf "$OUT/include"
mkdir -p "$OUT/include"
cp -r "$WORK/sdk/include/openxr" "$OUT/include/"
cp "$WORK/sdk/x64/bin/openxr_loader.dll" "$OUT/openxr_loader.dll"
cp "$WORK/sdk/share/doc/openxr/LICENSE" "$OUT/LICENSE"
echo "$OPENXR_VERSION" > "$OUT/VERSION"

echo "OpenXR SDK $OPENXR_VERSION fetched: $(ls "$OUT/include/openxr" | wc -l) headers, $OUT/openxr_loader.dll"
