#!/bin/bash
# Run the node harness against netduke32 off the real filesystem.
#   OPT=-O3 TPUT=1 ./run-node.sh          # WASM_TPUT -> FPSSAMPLE lines (fps/mips)
#   DUMP=1 ./run-node.sh                   # WASM_DUMP_FRAME -> base64 frames on stderr
#   HISTO=1 TPUT=1 ./run-node.sh           # opcode histogram
set -e
WINE="${WINE:-$HOME/dev/wine}"; WINEMAC="${WINEMAC:-$WINE/wine-macos}"
GAME="${GAME:-$HOME/games/netduke32_v1.2.1}"
OPT="${OPT:--O3}"; WORK="${WORK:-/tmp/webwine-browser}"
OUT="$WORK/nd_${OPT#-}"; ROOT="$WORK/fakeroot"; PREFIX="$WORK/prefix"; STAGE="$PREFIX/drive_c"
source ~/dev/emsdk/emsdk_env.sh >/dev/null 2>&1

if [ ! -d "$ROOT/lib/wine/i386-windows" ]; then
  rm -rf "$ROOT"; mkdir -p "$ROOT/lib/wine/i386-unix" "$ROOT/share/wine"
  ln -s "$WINEMAC/lib/wine/i386-windows" "$ROOT/lib/wine/i386-windows"
  ln -s "$WINEMAC/share/wine/nls" "$ROOT/share/wine/nls"
  [ -f "$WINEMAC/share/wine/wine.inf" ] && ln -s "$WINEMAC/share/wine/wine.inf" "$ROOT/share/wine/wine.inf" || true
  touch "$ROOT/lib/wine/i386-unix/ntdll.so"
fi
if [ ! -e "$STAGE/netduke32.exe" ]; then
  rm -rf "$PREFIX"; mkdir -p "$STAGE/windows" "$PREFIX/dosdevices"
  ln -s "$ROOT/lib/wine/i386-windows" "$STAGE/windows/system32"
  for f in netduke32.exe DUKE3D.GRP eduke32.dat; do ln -s "$GAME/$f" "$STAGE/$f"; done
  ln -sfn "$STAGE" "$PREFIX/dosdevices/c:"; ln -sfn "/" "$PREFIX/dosdevices/z:"
fi

cd "$STAGE"
export WINELOADERNOEXEC=1 WINE_NO_SERVER_SPAWN=1
export WINEUNIXDIR="$ROOT/lib/wine/i386-unix" WINEDLLPATH="$ROOT/lib/wine"
export WINEDATADIR="$WINEMAC/share/wine" WINEPREFIX="$PREFIX" WINE_START_CWD="$STAGE"
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy WINE_AUTO_ENTER=1
[ -n "$DUMP" ] && export WASM_DUMP_FRAME=1
[ -n "$TPUT" ] && export WASM_TPUT=1
[ -n "$HISTO" ] && export WASM_HISTO=1
ulimit -n 4096 2>/dev/null || true
exec node "$OUT/webwine.js" 'c:\netduke32.exe' "$@"
