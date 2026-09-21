#!/usr/bin/env bash
# Switch a Deus Ex install between the render devices in this repository and the
# stock Direct3D one.
#   select-renderer.sh vulkan|d3d11|d3d12|pathtracer|d3d [SystemDir]
set -euo pipefail

WHICH="${1:-}"
SYSTEM_DIR="${2:-$HOME/.local/share/Steam/steamapps/common/Deus Ex/System}"

case "$WHICH" in
	vulkan) DEV="VulkanDrv.VulkanRenderDevice" ;;
	d3d11)  DEV="D3D11Drv.D3D11RenderDevice" ;;
	d3d12)  DEV="D3D12Drv.D3D12RenderDevice" ;;
	pathtracer) DEV="PathTracerDrv.PathTracerRenderDevice" ;;
	d3d)    DEV="D3DDrv.D3DRenderDevice" ;;
	*) echo "usage: $0 vulkan|d3d11|d3d12|pathtracer|d3d [SystemDir]" >&2; exit 1 ;;
esac

# UE1 names its ini after the executable, so the retail DeusEx.exe from the
# 1112fm patch reads DeusExRetail.ini and the Steam one reads DeusEx.ini. Two
# copies of the same settings, and editing the wrong one changes nothing while
# looking like it worked. Write every one that exists.
written=0
for f in "$SYSTEM_DIR"/DeusEx.ini "$SYSTEM_DIR"/DeusExRetail.ini; do
	[ -f "$f" ] || continue
	sed -i "s|^GameRenderDevice=.*|GameRenderDevice=$DEV|" "$f"
	echo "$(basename "$f"): GameRenderDevice=$DEV"
	written=1
done
[ "$written" = 1 ] || { echo "No DeusEx ini found in $SYSTEM_DIR" >&2; exit 1; }
