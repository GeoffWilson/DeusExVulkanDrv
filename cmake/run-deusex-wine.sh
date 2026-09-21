#!/usr/bin/env bash
# Run Deus Ex under upstream wine rather than Proton.
#
#   run-deusex-wine.sh [SystemDir] [extra DeusEx.exe arguments...]
#
# Why this exists: Proton's winevulkan does not pass the ray tracing extensions
# through to a 32 bit client - every Proton build on this machine reports
# VK_KHR_ray_query as absent, while upstream wine reports it present on the same
# GPU. PathTracerDrv therefore cannot start under Proton and can under wine.
# Run cmake --build <dir> --target vkrtcheck and then this script's wine on the
# result to see what any particular setup offers.
#
# The Steam copy of the game has no Steam DRM wrapper, so it runs standalone. It
# shares its System folder, and therefore its DeusEx.ini, with the Proton copy:
# whichever renderer is selected there is the one that starts.
set -euo pipefail

SYSTEM_DIR="${1:-$HOME/.local/share/Steam/steamapps/common/Deus Ex/System}"
shift || true

[ -f "$SYSTEM_DIR/DeusEx.exe" ] || { echo "Not a Deus Ex System folder: $SYSTEM_DIR" >&2; exit 1; }

export WINEPREFIX="${WINEPREFIX:-$HOME/.wine-deusex}"
export WINEDEBUG="${WINEDEBUG:--all}"
export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-mscoree,mshtml=}"

if [ ! -d "$WINEPREFIX/drive_c" ]; then
	echo "Creating wine prefix at $WINEPREFIX"
	wineboot -u >/dev/null 2>&1
fi

# Steam's DeusEx.exe is a 64K launcher that only imports kernel32 and does
# nothing useful outside Steam. The retail one from the 1112fm patch is the
# genuine UE1 launcher - it imports Core.dll and Engine.dll directly - and runs
# with Steam nowhere in the picture. Extract it alongside as DeusExRetail.exe:
#   7z e DeusExMPPatch1112fm.exe ReleaseMPPatch1112fm/System/DeusEx.exe
EXE="DeusEx.exe"
[ -f "$SYSTEM_DIR/DeusExRetail.exe" ] && EXE="DeusExRetail.exe"

cd "$SYSTEM_DIR"
exec wine "$EXE" "$@"
