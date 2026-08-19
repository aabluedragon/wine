#!/bin/bash
# Build the native-WASM Wine (Option B) unix side + loader into one wasm module.
#
# This links ntdll's unix half (compiled to wasm by the build-wasm4 tree) with
# the wasm-host shims in this directory, producing a node-runnable wine that
# boots the Wine runtime as NATIVE WebAssembly (no x86 emulation of Wine
# itself).  It reaches the wineserver-connect step; wiring the in-process
# wineserver (wasm_ipc.c) and the PE CPU emulator (wasm_cpu_bridge.c seams) is
# the remaining work — see README.md.
#
# Prereqs: build-wasm4 configured + `make dlls/ntdll/ntdll.so` done (see README).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
WINE="$(cd "$HERE/.." && pwd)"
BUILD="${BUILD:-$WINE/build-wasm4}"
OUT="${OUT:-$HERE/out}"
source ~/dev/emsdk/emsdk_env.sh >/dev/null 2>&1
mkdir -p "$OUT"

INC="-Idlls/ntdll -I../dlls/ntdll -I../dlls/ntdll/unix -Iinclude -I../include"
CFLAGS="-std=gnu23 -D__WINESRC__ -D_NTSYSTEM_ -D_ACRTIMP= -DWINBASEAPI= -DWINE_UNIX_LIB \
  -Wall -pipe -fvisibility=hidden -fno-stack-protector -fno-strict-aliasing"

cd "$BUILD"
echo "[1/4] ntdll.so (native-wasm unix side)"
arch -x86_64 make dlls/ntdll/ntdll.so >/dev/null

echo "[2/4] host shims"
emcc -O1 -c "$HERE/wasm_vm.c"         -o "$OUT/wasm_vm.o"
emcc $CFLAGS $INC -c "$HERE/wasm_cpu_bridge.c" -o "$OUT/wasm_cpu_bridge.o"

echo "[3/4] link loader"
emcc -g -O1 "$HERE/wasm_loader_main.c" dlls/ntdll/ntdll.so \
  "$OUT/wasm_cpu_bridge.o" "$OUT/wasm_vm.o" \
  -o "$OUT/wine.js" -sNODERAWFS=1 -sASSERTIONS=1 \
  -sGLOBAL_BASE=1879048192 -sINITIAL_MEMORY=2147483648 -sALLOW_MEMORY_GROWTH=0

echo "[4/4] done -> $OUT/wine.js"
