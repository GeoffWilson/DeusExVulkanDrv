#!/usr/bin/env bash
# Build NVIDIA's NRD denoiser as a 32-bit Windows static library for the path
# tracer, from Linux.
#   build-nrd.sh [OutputDir]
#
# NRD is fetched at a pinned commit rather than kept in this repository: its
# licence does not allow its source to be redistributed here. Everything lands
# in OutputDir (default ../.nrd, beside the .xwin folder):
#   src/        NRD itself; src/Include is what the driver includes
#   x86/NRD.lib the library the path tracer links
#
# Two stages, because NRD's shaders are HLSL compiled by Microsoft's DXC, and
# DXC runs on the host:
#   1. a native build of NRD's shader target, which writes the SPIR-V the
#      library embeds and the NRDConfig.hlsli both halves must agree on;
#   2. the library's C++ compiled for i386 with clang-cl against the xwin CRT.
set -euo pipefail

NRD_COMMIT=7033ffbc48dbd74713194555abc13da8d7de4bcb   # NRD 4.18.0
DXC_URL=https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2607/linux_dxc_2026_07_29.x86_x64.tar.gz

REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$REPO/../.nrd}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
XWIN="${XWIN_DIR:-$REPO/../.xwin}"
[ -d "$XWIN/crt/include" ] || { echo "No xwin CRT at $XWIN (see cmake/README-crossbuild.md)" >&2; exit 1; }

# The settings the driver is written against. The normal encoding is NRD's
# default, octahedral normal and signed roughness, which the trace shader packs
# itself (the plain encodings do not compile in this version). Quad intrinsics
# would need VK_KHR_compute_shader_derivatives.
NRD_OPTIONS=(
	-DNRD_NORMAL_ENCODING=2
	-DNRD_ROUGHNESS_ENCODING=1
	-DNRD_SUPPORTS_QUAD_INTRINSICS=OFF
	-DNRD_SUPPORTS_VIEWPORT_OFFSET=OFF
	-DNRD_SUPPORTS_CHECKERBOARD=ON
	-DNRD_SUPPORTS_HISTORY_CONFIDENCE=ON
	-DNRD_SUPPORTS_DISOCCLUSION_THRESHOLD_MIX=ON
	-DNRD_SUPPORTS_ANTIFIREFLY=ON
	-DREBLUR_PERFORMANCE_MODE=OFF
)
# The same, as the library's compile definitions.
NRD_DEFINES=(
	-DNRD_SUPPORTS_QUAD_INTRINSICS=0
	-DNRD_SUPPORTS_VIEWPORT_OFFSET=0
	-DNRD_SUPPORTS_CHECKERBOARD=1
	-DNRD_SUPPORTS_HISTORY_CONFIDENCE=1
	-DNRD_SUPPORTS_DISOCCLUSION_THRESHOLD_MIX=1
	-DNRD_SUPPORTS_ANTIFIREFLY=1
	-DREBLUR_PERFORMANCE_MODE=0
	-DNRD_EMBEDS_SPIRV_SHADERS=1
	-DNRD_EMBEDS_DXIL_SHADERS=0
	-DNRD_EMBEDS_DXBC_SHADERS=0
	-DNRD_STATIC_LIBRARY=1
	-DSPIRV_SREG_OFFSET=0
	-DSPIRV_BREG_OFFSET=2
	-DSPIRV_UREG_OFFSET=3
	-DSPIRV_TREG_OFFSET=20
)

# Source, at the pinned commit.
if [ ! -d "$OUT/src/.git" ]; then
	git clone --quiet https://github.com/NVIDIA-RTX/NRD.git "$OUT/src"
fi
git -C "$OUT/src" fetch --quiet --depth 1 origin "$NRD_COMMIT" 2>/dev/null || true
git -C "$OUT/src" checkout --quiet --force "$NRD_COMMIT"

# Two declarations in NRD.h lack NRD_CALL. On x64 calling conventions are
# ignored; on x86 the definitions' __stdcall no longer matches them.
sed -i 's/NRD_API const char\* GetResourceTypeString/NRD_API const char* NRD_CALL GetResourceTypeString/; s/NRD_API const char\* GetDenoiserString/NRD_API const char* NRD_CALL GetDenoiserString/' "$OUT/src/Include/NRD.h"

# DXC, unpacked locally rather than installed.
DXC="$OUT/dxc/bin/dxc"
if [ ! -x "$DXC" ]; then
	mkdir -p "$OUT/dxc"
	curl -sL "$DXC_URL" | tar -xz -C "$OUT/dxc" --strip-components=1
fi

# Stage 1: shaders, natively.
cmake -S "$OUT/src" -B "$OUT/build-shaders" -G Ninja -DCMAKE_BUILD_TYPE=Release \
	-DNRD_STATIC_LIBRARY=ON -DNRD_EMBEDS_SPIRV_SHADERS=ON "${NRD_OPTIONS[@]}" \
	-DSHADERMAKE_DXC_VK_PATH="$DXC" > "$OUT/build-shaders.log"
cmake --build "$OUT/build-shaders" --target NRDShaders >> "$OUT/build-shaders.log"

DEPS="$OUT/build-shaders/_deps"

# Stage 2: the library, for i386.
mkdir -p "$OUT/x86/obj"
FLAGS=(--target=i386-pc-windows-msvc /std:c++17 /O2 /MT /EHsc -mssse3 /W0
	/imsvc"$XWIN/crt/include" /imsvc"$XWIN/sdk/include/ucrt" /imsvc"$XWIN/sdk/include/um" /imsvc"$XWIN/sdk/include/shared"
	-DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS -DNDEBUG "${NRD_DEFINES[@]}"
	-I"$OUT/src/Include" -I"$OUT/src/_Shaders" -I"$DEPS/mathlib-src" -I"$DEPS/shadermake-src")
# NRD's sources, and ShaderMake's blob reader, which picks each shader's
# permutation out of what NRD embeds.
for f in "$OUT/src/Source/"*.cpp "$DEPS/shadermake-src/ShaderMake/ShaderBlob.cpp"; do
	clang-cl "${FLAGS[@]}" /c "$f" /Fo"$OUT/x86/obj/$(basename "$f" .cpp).obj"
done
llvm-lib /out:"$OUT/x86/NRD.lib" "$OUT/x86/obj/"*.obj

echo "NRD $(git -C "$OUT/src" describe --always) built: $OUT/x86/NRD.lib"
