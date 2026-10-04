#!/usr/bin/env bash
# Fetch AMD's FSR 3.1 upscaler for the path tracer's helper, for the 64-bit
# Windows build, from Linux, and compile its shaders.
#   fetch-fsr.sh [OutputDir]
#
# FidelityFX SDK v1.1.4 is the last with a Vulkan backend: the SDKs after it
# (FSR 4) are DirectX 12 only. It is fetched at that tag rather than kept in
# this repository, as the DLSS SDK is, and only its sdk folder - the samples
# are most of a gigabyte. Into OutputDir (default ../.fsr, beside the .xwin
# folder):
#   sdk/        the SDK's sources and headers, of which the helper builds the
#               FSR 3.1 upscaler, the Vulkan backend and what they share
#   generated/  the upscaler's shaders as SPIR-V, in the permutation headers
#               the SDK's own build makes with its shader compiler
#
# The shader compiler is a Windows program, so it runs under wine. Each pass
# is compiled four times as the SDK does it - wave32 and wave64, with and
# without 16-bit maths - which for GLSL differ only in the 16-bit maths.
set -euo pipefail

FSR_TAG=v1.1.4
REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$REPO/../.fsr}"

if [ ! -d "$OUT/sdk/.git" ] && [ ! -d "$OUT/.git" ]; then
	git clone -q --filter=blob:none --sparse --depth 1 --branch "$FSR_TAG" \
		https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git "$OUT"
	git -C "$OUT" sparse-checkout set sdk
fi
OUT="$(cd "$OUT" && pwd)"
SDK="$OUT/sdk"
GEN="$OUT/generated"
mkdir -p "$GEN"

command -v wine >/dev/null || { echo "wine is needed to run the FidelityFX shader compiler" >&2; exit 1; }
SC="$SDK/tools/binary_store/FidelityFX_SC.exe"
GPU="Z:$SDK/include/FidelityFX/gpu"

# As sdk/include/FidelityFX/gpu/fsr3upscaler/CMakeCompileFSR3UpscalerShaders.txt
# and sdk/src/backends/vk/CMakeShadersFSR3Upscaler.txt have them.
BASE=(-reflection -deps=gcc -DFFX_GPU=1
	-DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0
	-DFFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0
	-DFFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1
	-DFFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0
	-DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2
	-compiler=glslang -e CS --target-env vulkan1.2 -S comp -Os -DFFX_GLSL=1
	"-DFFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE={0,1}"
	"-DFFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT={0,1}"
	"-DFFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS={0,1}"
	"-DFFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS={0,1}"
	"-DFFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH={0,1}"
	"-DFFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING={0,1}"
	"-I$GPU" "-I$GPU/fsr3upscaler" "-output=Z:$GEN")

count=0
for shader in "$SDK"/src/backends/vk/shaders/fsr3upscaler/*.glsl; do
	name="$(basename "$shader" .glsl)"
	for variant in "" "_wave64" "_16bit" "_wave64_16bit"; do
		[ -f "$GEN/${name}${variant}_permutations.h" ] && continue
		half=0
		case "$variant" in *16bit) half=1 ;; esac
		WINEDEBUG=-all wine "$SC" "${BASE[@]}" "-name=${name}${variant}" "-DFFX_HALF=$half" "Z:$shader" > /dev/null 2>&1 ||
			{ echo "the shader compiler failed on $name$variant" >&2; exit 1; }
		count=$((count + 1))
	done
done
echo "$FSR_TAG" > "$OUT/VERSION"

echo "FidelityFX SDK $FSR_TAG in $SDK: $count shader headers compiled, $(ls "$GEN"/*_permutations.h | wc -l) in $GEN"
