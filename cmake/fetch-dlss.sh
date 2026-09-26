#!/usr/bin/env bash
# Fetch the parts of NVIDIA's DLSS SDK the path tracer's helper uses, for the
# 64-bit Windows build, from Linux.
#   fetch-dlss.sh [OutputDir]
#
# The SDK is fetched at a pinned release rather than kept in this repository:
# like NRD's, its licence does not allow it to be redistributed here, and the
# Ray Reconstruction runtime alone is 48 MB. Only what the helper needs is
# downloaded, into OutputDir (default ../.dlss, beside the .xwin folder):
#   include/           the NGX headers
#   x64/nvsdk_ngx_s.lib the NGX loader, static CRT, as the helper is built
#   nvngx_dlssd.dll    Ray Reconstruction ("DLSS-D"), which ships beside the helper
#   LICENSE.txt        the NVIDIA RTX SDKs licence the above is used under
set -euo pipefail

DLSS_TAG=v310.9.1
BASE="https://raw.githubusercontent.com/NVIDIA/DLSS/$DLSS_TAG"

REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$REPO/../.dlss}"
mkdir -p "$OUT/include" "$OUT/x64"
OUT="$(cd "$OUT" && pwd)"

fetch() {
	curl -fsSL --retry 3 -o "$2" "$BASE/$1"
}

# Every header, as the SDK lists them at this release.
headers=$(gh api "repos/NVIDIA/DLSS/contents/include?ref=$DLSS_TAG" --jq '.[].name' 2>/dev/null ||
	curl -fsSL "https://api.github.com/repos/NVIDIA/DLSS/contents/include?ref=$DLSS_TAG" | grep -o '"name": *"[^"]*"' | cut -d'"' -f4)
for h in $headers; do
	fetch "include/$h" "$OUT/include/$h"
done
fetch lib/Windows_x86_64/x64/nvsdk_ngx_s.lib "$OUT/x64/nvsdk_ngx_s.lib"
fetch lib/Windows_x86_64/rel/nvngx_dlssd.dll "$OUT/nvngx_dlssd.dll"
fetch LICENSE.txt "$OUT/LICENSE.txt"
echo "$DLSS_TAG" > "$OUT/VERSION"

echo "DLSS SDK $DLSS_TAG fetched: $(ls "$OUT/include" | wc -l) headers, $OUT/x64/nvsdk_ngx_s.lib, $OUT/nvngx_dlssd.dll"
