#!/bin/bash
# Build a synthetic install tree for booting the wasm Wine under node.
# The unix dir MUST be named i386-unix (current_machine is i386 on wasm), so the
# loader's dll_dir math resolves the PE dir to <root>/lib/wine/i386-windows.
set -e
OUT="${1:-/tmp/webwine-root}"
WINEMAC="${WINEMAC:-$HOME/dev/wine/wine-macos}"
rm -rf "$OUT"; mkdir -p "$OUT/lib/wine/i386-unix" "$OUT/share/wine"
ln -s "$WINEMAC/lib/wine/i386-windows" "$OUT/lib/wine/i386-windows"   # PE builtins
ln -s "$WINEMAC/share/wine/nls" "$OUT/share/wine/nls"
touch "$OUT/lib/wine/i386-unix/ntdll.so"   # marker for path math (real ntdll.so is static-linked)
echo "fakeroot: $OUT"
echo "run: WINELOADERNOEXEC=1 WINE_NO_SERVER_SPAWN=1 WINEUNIXDIR=$OUT/lib/wine/i386-unix \\"
echo "     WINEDLLPATH=$OUT/lib/wine WINEDATADIR=$WINEMAC/share/wine WINEPREFIX=<prefix> \\"
echo "     node webwine/out/webwine.js ret42.exe"
