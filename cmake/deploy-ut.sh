#!/usr/bin/env bash
# Copy the cross-built PathTracerDrv for Unreal Tournament 469 into its System
# folder, and optionally point the game at it.
#   deploy-ut.sh [SystemDir] [pathtracer|vulkan|none]
#
# The 64-bit device traces in the game's own process, so there is no helper to
# copy: just the device, its .int, and DLSS Ray Reconstruction's runtime, which
# NGX looks for beside the executable. "none", the default, leaves
# GameRenderDevice as it is; the device can also be chosen in the game, under
# Preferences, Video.
set -euo pipefail

BUILD_DIR="${BUILD_DIR:-$(dirname "$0")/../build-ut469-x64}"
default_system_dir() {
	for d in \
		"$HOME/.local/share/Steam/steamapps/common/Unreal Tournament/System" \
		"$HOME/.steam/steam/steamapps/common/Unreal Tournament/System"
	do
		[ -f "$d/UnrealTournament.exe" ] && { echo "$d"; return; }
	done
	echo "$HOME/.local/share/Steam/steamapps/common/Unreal Tournament/System"
}
SYSTEM_DIR="${1:-$(default_system_dir)}"
SELECT="${2:-none}"
SRC="$(cd "$(dirname "$0")/.." && pwd)"

[ -f "$SYSTEM_DIR/UnrealTournament.exe" ] || { echo "Not an Unreal Tournament System folder: $SYSTEM_DIR" >&2; exit 1; }
[ -f "$BUILD_DIR/PathTracerDrv.dll" ] || { echo "No PathTracerDrv.dll in $BUILD_DIR" >&2; exit 1; }

# The device has to match the game: a 64-bit build for the 64-bit game.
if file "$SYSTEM_DIR/UnrealTournament.exe" | grep -q "x86-64"; then want="x86-64"; else want="80386"; fi
file "$BUILD_DIR/PathTracerDrv.dll" | grep -q "$want" ||
	{ echo "$BUILD_DIR/PathTracerDrv.dll is not built for this game's $want executable" >&2; exit 1; }

installed=()
copy() {
	cp "$1" "$SYSTEM_DIR/"
	# Hash checked rather than assumed: a copy that silently did not happen
	# looks exactly like a fix that did not work.
	if [ "$(md5sum < "$1")" != "$(md5sum < "$SYSTEM_DIR/$(basename "$1")")" ]; then
		echo "Deploy of $(basename "$1") did not land in $SYSTEM_DIR" >&2
		exit 1
	fi
	installed+=("$(basename "$1")")
}
copy "$BUILD_DIR/PathTracerDrv.dll"
copy "$SRC/PathTracerDrv.int"
[ -f "$BUILD_DIR/nvngx_dlssd.dll" ] && copy "$BUILD_DIR/nvngx_dlssd.dll"

case "$SELECT" in
	pathtracer) DEV="PathTracerDrv.PathTracerRenderDevice" ;;
	vulkan)     DEV="VulkanDrv.VulkanRenderDevice" ;;
	none)       DEV="" ;;
	*) echo "usage: $0 [SystemDir] [pathtracer|vulkan|none]" >&2; exit 1 ;;
esac

if [ -n "$DEV" ]; then
	INI="$SYSTEM_DIR/UnrealTournament.ini"
	[ -f "$INI.prepathtracer" ] || cp "$INI" "$INI.prepathtracer"
	# The ini is CRLF, and a value matched with .* takes the CR with it.
	CR=$'\r'
	sed -i "s|^GameRenderDevice=[^$CR]*|GameRenderDevice=$DEV|" "$INI"
	echo "UnrealTournament.ini: GameRenderDevice=$DEV (original kept as UnrealTournament.ini.prepathtracer)"
fi

echo "Installed ${installed[*]} into $SYSTEM_DIR"
