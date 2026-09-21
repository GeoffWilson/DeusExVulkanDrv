#!/usr/bin/env bash
# Switch a Deus Ex install between the render devices in this repository and the
# stock Direct3D one.
#   select-renderer.sh vulkan|d3d11|d3d12|pathtracer|d3d [SystemDir]
set -euo pipefail

WHICH="${1:-}"
SYSTEM_DIR="${2:-$HOME/.local/share/Steam/steamapps/common/Deus Ex/System}"
INI="$SYSTEM_DIR/DeusEx.ini"

case "$WHICH" in
	vulkan) DEV="VulkanDrv.VulkanRenderDevice" ;;
	d3d11)  DEV="D3D11Drv.D3D11RenderDevice" ;;
	d3d12)  DEV="D3D12Drv.D3D12RenderDevice" ;;
	pathtracer) DEV="PathTracerDrv.PathTracerRenderDevice" ;;
	d3d)    DEV="D3DDrv.D3DRenderDevice" ;;
	*) echo "usage: $0 vulkan|d3d11|d3d12|pathtracer|d3d [SystemDir]" >&2; exit 1 ;;
esac

sed -i "s|^GameRenderDevice=.*|GameRenderDevice=$DEV|" "$INI"
echo "GameRenderDevice=$DEV"
