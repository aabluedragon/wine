#!/bin/bash
# Build the single-module in-process wasm Wine: client (ntdll unix side) +
# wineserver + ring-buffer transport, all in ONE wasm module.  Boots both halves
# and exercises the client<->server handshake over the transport.
#
# The server's few globals that collide with ntdll's in one address space are
# namespaced with -D renames; server main() is renamed wineserver_main(); the
# 2 duplicate context helpers in server/thread.c are renamed to unused symbols
# (ntdll provides the live ones).  See README.md.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
WINE="$(cd "$HERE/.." && pwd)"
BUILD="${BUILD:-$WINE/build-wasm4}"
OUT="${OUT:-$HERE/out}"
source ~/dev/emsdk/emsdk_env.sh >/dev/null 2>&1
mkdir -p "$OUT/srv"
cd "$BUILD"

CF=(-std=gnu23 -Iserver -I../server -Iinclude -I../include -D__WINESRC__ -DWINE_UNIX_LIB
    -fvisibility=hidden -fno-stack-protector -fno-strict-aliasing)
# rename server globals that collide with ntdll in a shared address space
R=(-Duser_shared_data=srv_user_shared_data
   -Dserver_start_time=srv_server_start_time
   -Dnative_machine=srv_native_machine
   -Dsupported_machines=srv_supported_machines
   -Dsupported_machines_count=srv_supported_machines_count
   -Dget_thread_context=server_unused_get_thread_context
   -Dset_thread_context=server_unused_set_thread_context)

echo "[1/5] ntdll.so + server objects (native wasm)"
arch -x86_64 make dlls/ntdll/ntdll.so >/dev/null
arch -x86_64 make server/wineserver 2>/dev/null || true   # compiles the .o's

echo "[2/5] server objects with namespaced globals"
for c in ../server/*.c; do
  b=$(basename "$c" .c)
  emcc "${CF[@]}" "${R[@]}" -c "$c" -o "$OUT/srv/$b.o" 2>/dev/null \
    || cp "server/$b.o" "$OUT/srv/$b.o"   # unicode.c has no renamed refs
done

echo "[3/5] host shims + harness"
INC="-Idlls/ntdll -I../dlls/ntdll -I../dlls/ntdll/unix -Iinclude -I../include"
CF2="-D__WINESRC__ -D_NTSYSTEM_ -D_ACRTIMP= -DWINBASEAPI= -DWINE_UNIX_LIB -fvisibility=hidden -fno-stack-protector -fno-strict-aliasing"
emcc -O1 -c "$HERE/wasm_vm.c"  -o "$OUT/wasm_vm.o"
emcc -O1 -c "$HERE/wasm_ipc.c" -o "$OUT/wasm_ipc.o"
emcc -std=gnu23 $CF2 $INC -c "$HERE/wasm_cpu_bridge.c" -o "$OUT/wasm_cpu_bridge.o"
emcc -O1 -c "$HERE/wasm_combined_main.c" -o "$OUT/combined_main.o"

echo "[4/5] link one module"
: > "$OUT/objs.rsp"
echo "$OUT/combined_main.o" >> "$OUT/objs.rsp"
ls "$OUT"/srv/*.o           >> "$OUT/objs.rsp"
echo "dlls/ntdll/ntdll.so"  >> "$OUT/objs.rsp"
echo "$OUT/wasm_cpu_bridge.o" >> "$OUT/objs.rsp"
echo "$OUT/wasm_vm.o"       >> "$OUT/objs.rsp"
echo "$OUT/wasm_ipc.o"      >> "$OUT/objs.rsp"

WRAPS="-Wl,--wrap=read -Wl,--wrap=write -Wl,--wrap=close -Wl,--wrap=poll -Wl,--wrap=fcntl -Wl,--wrap=sendmsg -Wl,--wrap=recvmsg"
emcc -g -O1 @"$OUT/objs.rsp" $WRAPS -o "$OUT/webwine.js" \
  -sNODERAWFS=1 -sASSERTIONS=1 \
  -sGLOBAL_BASE=1879048192 -sINITIAL_MEMORY=2147483648 -sALLOW_MEMORY_GROWTH=0

echo "[5/5] done -> $OUT/webwine.js"
echo "run: WINELOADERNOEXEC=1 WINE_NO_SERVER_SPAWN=1 WINEUNIXDIR=<fakeroot>/lib/wine/wasm32-unix \\"
echo "     WINEDLLPATH=<fakeroot>/lib/wine WINEDATADIR=<wine>/share/wine WINEPREFIX=<prefix> \\"
echo "     node $OUT/webwine.js ret42.exe"
