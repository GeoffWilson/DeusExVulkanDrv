#!/usr/bin/env bash
# Copy the cross-built drivers into a Deus Ex System folder and point the game at one.
#   deploy-deusex.sh [SystemDir] [vulkan|d3d11|d3d12|pathtracer|none]
#
# Every driver that was built is installed; the second argument only decides
# which one the ini selects, and "none" leaves GameRenderDevice alone.
set -euo pipefail

BUILD_DIR="${BUILD_DIR:-$(dirname "$0")/../build-deusex}"
# Default to wherever the game actually is. The Steam install is the one that
# gets played; defaulting to the wine prefix made a deploy report success while
# copying the driver somewhere the running game never reads.
default_system_dir() {
	for d in \
		"$HOME/.local/share/Steam/steamapps/common/Deus Ex/System" \
		"$HOME/.steam/steam/steamapps/common/Deus Ex/System" \
		"$HOME/.wine/drive_c/drive_c/DeusEx/System"
	do
		[ -f "$d/DeusEx.exe" ] && { echo "$d"; return; }
	done
	echo "$HOME/.wine/drive_c/drive_c/DeusEx/System"
}
SYSTEM_DIR="${1:-$(default_system_dir)}"
SELECT="${2:-vulkan}"
SRC="$(cd "$(dirname "$0")/.." && pwd)"

[ -f "$SYSTEM_DIR/DeusEx.exe" ] || { echo "Not a Deus Ex System folder: $SYSTEM_DIR" >&2; exit 1; }

installed=()
for drv in VulkanDrv D3D11Drv D3D12Drv PathTracerDrv; do
	if [ -f "$BUILD_DIR/$drv.dll" ]; then
		cp "$BUILD_DIR/$drv.dll" "$SYSTEM_DIR/"
		# Hash checked rather than assumed: a copy that silently did not happen
		# looks exactly like a fix that did not work.
		if [ "$(md5sum < "$BUILD_DIR/$drv.dll")" != "$(md5sum < "$SYSTEM_DIR/$drv.dll")" ]; then
			echo "Deploy of $drv.dll did not land in $SYSTEM_DIR" >&2
			exit 1
		fi
		[ -f "$SRC/$drv.int" ] && cp "$SRC/$drv.int" "$SYSTEM_DIR/"
		installed+=("$drv")
	fi
done

# PathTracerDrv traces in a 64-bit helper beside it, built by the 64-bit
# configuration (see cmake/README-crossbuild.md). Without it the device will
# not start.
HELPER_DIR="${HELPER_DIR:-$(dirname "$0")/../build-x64}"
if [ -f "$BUILD_DIR/PathTracerDrv.dll" ]; then
	if [ -f "$HELPER_DIR/PathTracerHelper.exe" ]; then
		cp "$HELPER_DIR/PathTracerHelper.exe" "$SYSTEM_DIR/"
		if [ "$(md5sum < "$HELPER_DIR/PathTracerHelper.exe")" != "$(md5sum < "$SYSTEM_DIR/PathTracerHelper.exe")" ]; then
			echo "Deploy of PathTracerHelper.exe did not land in $SYSTEM_DIR" >&2
			exit 1
		fi
		installed+=("PathTracerHelper")
	else
		echo "No PathTracerHelper.exe in $HELPER_DIR: PathTracerDrv will not start without it" >&2
	fi
fi

[ ${#installed[@]} -gt 0 ] || { echo "Nothing built in $BUILD_DIR" >&2; exit 1; }

# UE1 names its ini after the executable: DeusEx.exe reads DeusEx.ini and the
# retail DeusExRetail.exe reads DeusExRetail.ini. Editing only one of them
# changes nothing while appearing to succeed.
# An array, not a string: the Steam path contains a space, and word splitting a
# joined string turns "Deus Ex" into two nonexistent paths.
INIS=()
for f in "$SYSTEM_DIR/DeusEx.ini" "$SYSTEM_DIR/DeusExRetail.ini"; do
	[ -f "$f" ] && INIS+=("$f")
done
[ ${#INIS[@]} -gt 0 ] || { echo "No DeusEx ini found in $SYSTEM_DIR" >&2; exit 1; }
for f in "${INIS[@]}"; do
	[ -f "$f.prevulkan" ] || cp "$f" "$f.prevulkan"
done

case "$SELECT" in
	vulkan) DEV="VulkanDrv.VulkanRenderDevice" ;;
	d3d11)  DEV="D3D11Drv.D3D11RenderDevice" ;;
	d3d12)  DEV="D3D12Drv.D3D12RenderDevice" ;;
	pathtracer) DEV="PathTracerDrv.PathTracerRenderDevice" ;;
	none)   DEV="" ;;
	*) echo "usage: $0 [SystemDir] [vulkan|d3d11|d3d12|pathtracer|none]" >&2; exit 1 ;;
esac

if [ -n "$DEV" ]; then
	for f in "${INIS[@]}"; do
		sed -i "s|^GameRenderDevice=.*|GameRenderDevice=$DEV|" "$f"
		# These devices present through Vulkan or D3D and never through
		# DirectDraw, but the engine still asks DirectDraw for a real display
		# mode change when going fullscreen. Under wine that fails and the engine
		# gives up with EndFullscreen the moment it has entered.
		sed -i "s|^UseDirectDraw=.*|UseDirectDraw=False|" "$f"
		echo "$(basename "$f"): GameRenderDevice=$DEV, UseDirectDraw=False"
	done
fi

echo "Installed ${installed[*]} into $SYSTEM_DIR (original ini kept as DeusEx.ini.prevulkan)"
