#!/bin/bash
# Assemble the MEMFS preload tree for the browser bundle: the PE-DLL import
# closure of netduke32.exe (+ 4 forward targets), the game data, and the NLS
# tables.  ~176MB.  Output: $WORK/assets/{game,root}.
#
#   WORK=/tmp/webwine-browser GAME=~/games/netduke32_v1.2.1 ./assemble-assets.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="${WORK:-/tmp/webwine-browser}"
WINE="${WINE:-$HOME/dev/wine}"
WINEMAC="${WINEMAC:-$WINE/wine-macos}"
GAME="${GAME:-$HOME/games/netduke32_v1.2.1}"
PEDIR="$WINEMAC/lib/wine/i386-windows"
AT="$WORK/assets"

rm -rf "$AT"
mkdir -p "$AT/game" "$AT/root/lib/wine/i386-windows" "$AT/root/lib/wine/i386-unix" "$AT/root/share/wine"

# BFS import closure from the exe + the 4 forward targets not in any import table
# (apisetschema: api-ms-win redirects; cryptbase: advapi32.SystemFunction036).
DLLS="$(GAME="$GAME" PEDIR="$PEDIR" python3 "$HERE/dll-closure.py" 2>/dev/null) apisetschema.dll cryptbase.dll cryptsp.dll bcrypt.dll"
n=0
for d in $DLLS; do
  if [ -f "$PEDIR/$d" ]; then cp "$PEDIR/$d" "$AT/root/lib/wine/i386-windows/"; n=$((n+1)); else echo "MISSING $d"; fi
done
echo "copied $n PE dlls"

for f in netduke32.exe DUKE3D.GRP eduke32.dat; do cp "$GAME/$f" "$AT/game/"; done
# Render the classic view at half resolution and upscale — ~2x FPS in the browser.
printf 'r_upscalefactor 2\n' > "$AT/game/autoexec.cfg"

touch "$AT/root/lib/wine/i386-unix/ntdll.so"          # loader path-math marker
cp -R "$WINEMAC/share/wine/nls" "$AT/root/share/wine/nls"
[ -f "$WINEMAC/share/wine/wine.inf" ] && cp "$WINEMAC/share/wine/wine.inf" "$AT/root/share/wine/" || true

echo "assets -> $AT"; du -sh "$AT"
