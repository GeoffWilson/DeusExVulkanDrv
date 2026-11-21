#!/usr/bin/env bash
# Fetch AMD's FSR 4 upscaler for the path tracer's helper: FidelityFX SDK
# v2.4.0, the first with FSR 4 on Vulkan.
#   fetch-fsr4.sh [OutputDir]
#
# Unlike FSR 3.1 (cmake/fetch-fsr.sh), FSR 4 comes only as AMD's signed
# 64-bit DLLs, loaded at run time through the loader DLL; nothing is compiled.
# Only what that needs is checked out, into OutputDir (default ../.fsr4,
# beside ../.fsr):
#   Kits/FidelityFX/api/include        the ffx-api headers
#   Kits/FidelityFX/upscalers/include  the upscaler's descriptors
#   Kits/FidelityFX/signedbin/*_vk.*   amd_fidelityfx_loader_vk.dll and
#                                      amd_fidelityfx_upscaler_vk.dll (36 MB)
#   Kits/FidelityFX/docs/license.md    the licence the DLLs ship under, which
#                                      must go with them
set -euo pipefail

FSR4_TAG=v2.4.0
REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$REPO/../.fsr4}"

if [ ! -d "$OUT/.git" ]; then
	git clone -q --filter=blob:none --no-checkout --depth 1 --branch "$FSR4_TAG" \
		https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git "$OUT"
fi
git -C "$OUT" sparse-checkout set --no-cone \
	'/Kits/FidelityFX/api/include/' \
	'/Kits/FidelityFX/upscalers/include/' \
	'/Kits/FidelityFX/signedbin/amd_fidelityfx_loader_vk.dll' \
	'/Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_vk.dll' \
	'/Kits/FidelityFX/docs/license.md'
git -C "$OUT" checkout -q "$FSR4_TAG"
echo "FSR 4 ($FSR4_TAG) in $(cd "$OUT" && pwd)"
