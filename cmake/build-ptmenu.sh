#!/usr/bin/env bash
# Compile PathTracerMenu.u, the game's menus with the path tracer's settings
# added, with the Deus Ex SDK's UCC under wine.
#   build-ptmenu.sh [GameDir]
#
# UCC compiles against the game's own packages, so it runs in a System folder
# of links to the game's files beside copies of its inis, DeusEx.ini
# listing this package; the game's folder itself is only read. The package lands in
# build-deusex, where deploy-deusex.sh finds it.
set -euo pipefail

SRC="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$SRC/build-deusex}"
UCC="${UCC:-$SRC/../ReleaseSDK1112f/System/UCC.exe}"
GAME_DIR="${1:-$HOME/.local/share/Steam/steamapps/common/Deus Ex}"

[ -f "$GAME_DIR/System/DeusEx.u" ] || { echo "Not a Deus Ex folder: $GAME_DIR" >&2; exit 1; }
[ -f "$UCC" ] || { echo "No UCC.exe at $UCC (set UCC=)" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/System" "$WORK/PathTracerMenu"
for f in "$GAME_DIR/System/"*; do
	case "$(basename "$f")" in
		*.log|PathTracerMenu.*) ;;
		*.ini) cp "$f" "$WORK/System/" ;;
		*) ln -s "$f" "$WORK/System/" ;;
	esac
done
# The packages the game's scripts import their textures and sounds from.
for d in Textures Sounds Music Maps; do
	[ -d "$GAME_DIR/$d" ] && ln -s "$GAME_DIR/$d" "$WORK/$d"
done
cp "$UCC" "$WORK/System/UCC.exe"
cp -r "$SRC/PathTracerMenu/Classes" "$WORK/PathTracerMenu/"

# The inis are CRLF. The package goes after the game's own, which it uses.
CR=$'\r'
INI="$WORK/System/DeusEx.ini"
sed -i "/^EditPackages=PathTracerMenu/d; s|^EditPackages=DeusEx$CR\$|EditPackages=DeusEx$CR\nEditPackages=PathTracerMenu$CR|" "$INI"
grep -q "^EditPackages=PathTracerMenu" "$INI" || { echo "No EditPackages=DeusEx line in $GAME_DIR/System/DeusEx.ini" >&2; exit 1; }

(cd "$WORK/System" && WINEDEBUG=-all wine UCC.exe make 2>&1 | grep -v "MESA\|pci id" | grep -iE "error|warning|success|failed" || true)
[ -f "$WORK/System/PathTracerMenu.u" ] || { echo "UCC made no PathTracerMenu.u" >&2; exit 1; }
mkdir -p "$BUILD_DIR"
cp "$WORK/System/PathTracerMenu.u" "$BUILD_DIR/"
echo "Built $BUILD_DIR/PathTracerMenu.u"
