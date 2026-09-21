#!/usr/bin/env bash
# Copy the cross-built drivers into a Deus Ex System folder and point the game at one.
#   deploy-deusex.sh [SystemDir] [vulkan|d3d11|d3d12|pathtracer|none]
#
# Every driver that was built is installed; the second argument only decides
# which one the ini selects, and "none" leaves GameRenderDevice alone.
set -euo pipefail

BUILD_DIR="${BUILD_DIR:-$(dirname "$0")/../build-deusex}"
SYSTEM_DIR="${1:-$HOME/.wine/drive_c/drive_c/DeusEx/System}"
SELECT="${2:-vulkan}"
SRC="$(cd "$(dirname "$0")/.." && pwd)"

[ -f "$SYSTEM_DIR/DeusEx.exe" ] || { echo "Not a Deus Ex System folder: $SYSTEM_DIR" >&2; exit 1; }

installed=()
for drv in VulkanDrv D3D11Drv D3D12Drv PathTracerDrv; do
	if [ -f "$BUILD_DIR/$drv.dll" ]; then
		cp "$BUILD_DIR/$drv.dll" "$SYSTEM_DIR/"
		[ -f "$SRC/$drv.int" ] && cp "$SRC/$drv.int" "$SYSTEM_DIR/"
		installed+=("$drv")
	fi
done

[ ${#installed[@]} -gt 0 ] || { echo "Nothing built in $BUILD_DIR" >&2; exit 1; }

INI="$SYSTEM_DIR/DeusEx.ini"
[ -f "$INI.prevulkan" ] || cp "$INI" "$INI.prevulkan"

case "$SELECT" in
	vulkan) DEV="VulkanDrv.VulkanRenderDevice" ;;
	d3d11)  DEV="D3D11Drv.D3D11RenderDevice" ;;
	d3d12)  DEV="D3D12Drv.D3D12RenderDevice" ;;
	pathtracer) DEV="PathTracerDrv.PathTracerRenderDevice" ;;
	none)   DEV="" ;;
	*) echo "usage: $0 [SystemDir] [vulkan|d3d11|d3d12|pathtracer|none]" >&2; exit 1 ;;
esac

if [ -n "$DEV" ]; then
	sed -i "s|^GameRenderDevice=.*|GameRenderDevice=$DEV|" "$INI"
	echo "GameRenderDevice=$DEV"
fi

echo "Installed ${installed[*]} into $SYSTEM_DIR (original ini kept as DeusEx.ini.prevulkan)"
