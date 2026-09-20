#!/usr/bin/env bash
# Copy the cross-built driver into a Deus Ex System folder and point the game at it.
set -euo pipefail

BUILD_DIR="${BUILD_DIR:-$(dirname "$0")/../build-deusex}"
SYSTEM_DIR="${1:-$HOME/.wine/drive_c/drive_c/DeusEx/System}"
SRC="$(cd "$(dirname "$0")/.." && pwd)"

[ -f "$SYSTEM_DIR/DeusEx.exe" ] || { echo "Not a Deus Ex System folder: $SYSTEM_DIR" >&2; exit 1; }

cp "$BUILD_DIR/VulkanDrv.dll" "$SYSTEM_DIR/"
cp "$SRC/VulkanDrv.int" "$SYSTEM_DIR/"

INI="$SYSTEM_DIR/DeusEx.ini"
[ -f "$INI.prevulkan" ] || cp "$INI" "$INI.prevulkan"
sed -i 's|^GameRenderDevice=.*|GameRenderDevice=VulkanDrv.VulkanRenderDevice|' "$INI"

echo "Installed VulkanDrv into $SYSTEM_DIR (original ini kept as DeusEx.ini.prevulkan)"
