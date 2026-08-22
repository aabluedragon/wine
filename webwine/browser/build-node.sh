#!/bin/bash
# Node NODERAWFS harness for native-wasm Wine + the i386 interpreter — the fast
# measurement loop (no MEMFS preload; reads the game off the real filesystem).
# Also produces the env-agnostic objects (srv/*.o, wasm_cpu_bridge.o, wasm_vm.o,
# combined_main.o) that build-browser.sh reuses.
#
#   OPT=-O1 WORK=/tmp/webwine-browser ./build-node.sh
# NOTE: run under bash, not zsh (zsh does not word-split $DLLS/$INC).
# Interpreter opt is XOPT, default -O2.  Once the cold x87/SSE blocks were split
# out of run(), -O2 beat -O1 by ~11%; BEFORE that split -O1 won.  See README.md.
set -e
WINE="${WINE:-$HOME/dev/wine}"; BUILD="$WINE/build-wasm4"; WEBW="$WINE/webwine"
OPT="${OPT:--O1}"
# The interpreter itself wants -O2; see the perf notes in README.md.
XOPT="${XOPT:--O2}"
WORK="${WORK:-/tmp/webwine-browser}"
OUT="$WORK/nd_${OPT#-}"
source ~/dev/emsdk/emsdk_env.sh >/dev/null 2>&1
mkdir -p "$OUT/srv"; cd "$BUILD"

CF=(-std=gnu23 -Iserver -I../server -Iinclude -I../include -D__WINESRC__ -DWINE_UNIX_LIB
    -fvisibility=hidden -fno-stack-protector -fno-strict-aliasing)
R=(-Duser_shared_data=srv_user_shared_data -Dserver_start_time=srv_server_start_time
   -Dnative_machine=srv_native_machine -Dsupported_machines=srv_supported_machines
   -Dsupported_machines_count=srv_supported_machines_count
   -Dget_thread_context=server_unused_get_thread_context
   -Dset_thread_context=server_unused_set_thread_context)

echo "[1/4] ntdll.so + server objects"
arch -x86_64 make dlls/ntdll/ntdll.so >/dev/null
arch -x86_64 make server/wineserver 2>/dev/null || true
for c in ../server/*.c; do b=$(basename "$c" .c)
  emcc "${CF[@]}" "${R[@]}" -c "$c" -o "$OUT/srv/$b.o" 2>/dev/null || cp "server/$b.o" "$OUT/srv/$b.o"; done

echo "[2/4] shims ($OPT) + interpreter ($XOPT)"
INC="-Idlls/ntdll -I../dlls/ntdll -I../dlls/ntdll/unix -Iinclude -I../include"
CF2="-D__WINESRC__ -D_NTSYSTEM_ -D_ACRTIMP= -DWINBASEAPI= -DWINE_UNIX_LIB -fvisibility=hidden -fno-stack-protector -fno-strict-aliasing"
emcc $OPT -c "$WEBW/wasm_vm.c"  -o "$OUT/wasm_vm.o"
emcc $OPT -c "$WEBW/wasm_ipc.c" -o "$OUT/wasm_ipc.o"
emcc $OPT -c "$WEBW/wasm_egl_stubs.c" -o "$OUT/wasm_egl_stubs.o"   # EGL entry points emscripten lacks
emcc -std=gnu23 $OPT $CF2 $INC -c "$WEBW/wasm_cpu_bridge.c" -o "$OUT/wasm_cpu_bridge.o"
emcc -std=gnu23 $XOPT $CF2 $INC -c "$WEBW/wasm_x86.c"       -o "$OUT/wasm_x86.o"
emcc $OPT -c "$WEBW/wasm_combined_main.c" -o "$OUT/combined_main.o"

# opengl32's unix companion.  Compiled here rather than taken from
# dlls/opengl32/opengl32.so because its dispatch table has to be renamed: every
# unix companion defines __wine_unix_call_funcs and they all land in one
# statically linked module (ntdll/unix/virtual.c picks it up by the new name).
OGLINC="-Idlls/opengl32 -I../dlls/opengl32 -Iinclude -I../include"
OGLCF="-D__WINESRC__ -D_OPENGL32_ -DWINE_UNIX_LIB -fvisibility=hidden -fno-stack-protector -fno-strict-aliasing"
for c in unix_thunks unix_wgl; do
  emcc -std=gnu23 $OPT $OGLCF $OGLINC -D__wine_unix_call_funcs=__wine_unix_call_funcs_opengl32 \
       -c "../dlls/opengl32/$c.c" -o "$OUT/ogl_$c.o"
done

echo "[3/4] response file"
RSP="$OUT/objs.rsp"; : > "$RSP"
echo "$OUT/combined_main.o" >> "$RSP"; ls "$OUT"/srv/*.o >> "$RSP"
for so in dlls/ntdll/ntdll.so dlls/win32u/win32u.so dlls/ws2_32/ws2_32.so; do echo "$so" >> "$RSP"; done
for o in wasm_cpu_bridge wasm_x86 wasm_vm wasm_ipc; do echo "$OUT/$o.o" >> "$RSP"; done
for o in ogl_unix_thunks ogl_unix_wgl wasm_egl_stubs; do echo "$OUT/$o.o" >> "$RSP"; done

echo "[4/4] link ($OPT, no -g, no ASSERTIONS; 64MB stack for the re-entrant interpreter)"
emcc $OPT @"$RSP" -o "$OUT/webwine.js" \
  -sNODERAWFS=1 -sENVIRONMENT=node \
  -lEGL -lGLESv2 -sMAX_WEBGL_VERSION=2 -sGL_ENABLE_GET_PROC_ADDRESS=1 \
  -sGLOBAL_BASE=1879048192 -sINITIAL_MEMORY=2147483648 -sALLOW_MEMORY_GROWTH=0 -sSTACK_SIZE=67108864
echo "done -> $OUT/webwine.js"
