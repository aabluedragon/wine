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
# FPS config, executed by the game at startup (see README perf notes):
#   vidmode 640x400 - the window the engine presents; the default 1024x768 costs
#     ~2.5x the pixels for no visible benefit at this scale.
#   r_upscalefactor - render the classic view at 1/N and upscale.  The engine
#     clamps the result to its 320x200 floor, so 3 lands on 320x200 here.
#   r_maxfps/r_vsync - make sure nothing caps the frame rate.
#   snd_mixrate/snd_numvoices - the mixer and the OPL3 music synth are guest code
#     run under the interpreter, so they are expensive here; 22050Hz and 16 voices
#     sound fine for this game and cost roughly half of 44.1kHz/96.
cat > "$AT/game/autoexec.cfg" <<'CFG'
vidmode 640 400 8 0
r_upscalefactor 3
r_maxfps 0
r_vsync 0
snd_mixrate 22050
snd_numvoices 16
mus_enabled 0
CFG

touch "$AT/root/lib/wine/i386-unix/ntdll.so"          # loader path-math marker
cp -R "$WINEMAC/share/wine/nls" "$AT/root/share/wine/nls"
[ -f "$WINEMAC/share/wine/wine.inf" ] && cp "$WINEMAC/share/wine/wine.inf" "$AT/root/share/wine/" || true

echo "assets -> $AT"; du -sh "$AT"
