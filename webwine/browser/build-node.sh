#!/bin/bash
# Node NODERAWFS harness for native-wasm Wine + the i386 interpreter — the fast
# measurement loop (no MEMFS preload; reads the game off the real filesystem).
# Also produces the env-agnostic objects (srv/*.o, wasm_cpu_bridge.o, wasm_vm.o,
# combined_main.o) that build-browser.sh reuses.
#
#   OPT=-O3 XOPT=-O3 WORK=/tmp/webwine-browser ./build-node.sh
# NOTE: run under bash, not zsh (zsh does not word-split $DLLS/$INC).
# The browser build defaults to -O3 for support objects and the
# interpreter/generated blocks.  This is the measured fast configuration that
# preserves the interpreter's differential-verification behavior; override
# either variable for diagnostic or size-oriented builds.
set -e
WINE="${WINE:-$HOME/dev/wine}"; WINEMAC="${WINEMAC:-$WINE/wine-macos}"; BUILD="$WINE/build-wasm4"; WEBW="$WINE/webwine"
OPT="${OPT:--O3}"
XOPT="${XOPT:--O3}"
LINKOPT="${LINKOPT:-$OPT}"
WORK="${WORK:-/tmp/webwine-browser}"
OUT="$WORK/nd_${OPT#-}"
source ~/dev/emsdk/emsdk_env.sh >/dev/null 2>&1

# Keep the normal node artifact aligned with the browser artifact. A generated
# block table is part of the shipped performance build; requiring callers to
# remember GENBLK=1 made it too easy to overwrite nd_O3 with an interpreter-only
# binary while still reporting the same output path. An explicit GENBLK=0
# remains available for diagnostics.
if [ -z "${GENBLK+x}" ] && [ -f "$WEBW/gen_blocks.c" ]; then GENBLK=1; fi

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
# AOT x86->C block translator (GENBLK=1): regenerate gen_blocks.c from the game
# binary.  It is a build artifact (~40MB, ~1M lines) - generated, never committed.
# The hook list must match the guest addresses wasm_x86.c registers via nat_arm_*
# (so those functions stay native fast paths and are never mid-block-translated);
# the excluded 0x631000-0x634000 window is the SMC mapper region, with only
# the non-SMC cache1d lookup slice explicitly included below.
if [ -n "$GENBLK" ]; then
  NDEXE="${NDEXE:-$HOME/games/netduke32_v1.2.1/netduke32.exe}"
  NDHOOKS='--hooks=0x401e60,0x4e28a0,0x4e3070,0x519620,0x5196c0,0x519a41,0x519f87,0x529500,0x52a6a0,0x594550,0x60b9c0,0x6a8140,0x6a8170,0x6a81a0,0x6a81b0,0x6a8240,0x6a8250,0x6a8260,0x6a8270,0x6a8280,0x6a8290,0x6a8420,0x6a8c80'
  NDENTRIES="${NDENTRIES:---entries=0x004e73b0,0x00506c30,0x00572cd0,0x0057aa20,0x006b0c80,0x006b12f0,0x006b1650,0x006f33a4,0x007007b0,0x0070d7a0,0x0073cb20,0x008011a0,0x00801231,0x00801232,0x00801310,0x0080da90}"
  NDARGS=(); [ -z "$NDENTRIES" ] || NDARGS+=("$NDENTRIES")
  if [ ! -f "$WEBW/gen_blocks.c" ] || [ "$WEBW/x86toc.py" -nt "$WEBW/gen_blocks.c" ] || [ "$NDEXE" -nt "$WEBW/gen_blocks.c" ]; then
    echo "  [genblocks] translating $NDEXE -> gen_blocks.c"
    # The mapper body remains excluded because it self-modifies, but its fixed
    # post-loop epilogue is ordinary code.  Translating that 43-byte tail
    # removes the native mapper -> interpreter boundary without making any
    # assumptions about the caller's stack layout.
  python3 "$WEBW/x86toc.py" "$NDEXE" "$WEBW/gen_blocks.c" 0x401000-0x631000 0x6315f0-0x631750 0x631c40-0x631c90 0x632630-0x63265b 0x634000-0x81f710 "$NDHOOKS" "${NDARGS[@]}" --entries=0x006315f0,0x00631c40,0x00631c60,0x00631c7d
  fi
  if [ -n "$FP_HOT" ]; then
    FPHOT_GENFILE="$WEBW/fp_hot_gen_blocks.c"
    FPHOT_RANGE="${FP_HOT_RANGE:-0x500000-0x540000}"
    if [ ! -f "$FPHOT_GENFILE" ] || [ "$WEBW/x86toc.py" -nt "$FPHOT_GENFILE" ] || [ "$NDEXE" -nt "$FPHOT_GENFILE" ]; then
      echo "  [genblocks] translating floating-point render hot range"
      python3 "$WEBW/x86toc.py" "$NDEXE" "$FPHOT_GENFILE" \
        "$FPHOT_RANGE" --prefix=fp_ "$NDHOOKS"
    fi
  fi
  # GDI32's WidenPath entry is a separately relocated Wine DLL hotspot.
  # Keep this candidate narrow: the full exported CFG is not safe to dispatch
  # speculatively because it contains unrelated dynamic-call paths.
  GDI32="$WINEMAC/lib/wine/i386-windows/gdi32.dll"
  GDI32_GENFILE="$WEBW/gdi32_gen_blocks.c"
  if [ ! -f "$GDI32_GENFILE" ] || [ "$WEBW/x86toc.py" -nt "$GDI32_GENFILE" ] || [ "$GDI32" -nt "$GDI32_GENFILE" ]; then
    echo "  [genblocks] translating gdi32 WidenPath candidate"
    python3 "$WEBW/x86toc.py" "$GDI32" "$GDI32_GENFILE" \
      0x10042f10-0x10042fc0 --slide-symbol=gdi32_slide --prefix=gdi32_ --no-fp --entries=0x10042f10
  fi
  if [ -n "$MSVCRT_AOT" ]; then
    MSVCRT="$WINEMAC/lib/wine/i386-windows/msvcrt.dll"
    MSVCRT_GENFILE="$WEBW/msvcrt_gen_blocks.c"
    [ -n "$MSVCRT_AOT_RANGE" ] && MSVCRT_GENFILE="$WEBW/msvcrt_focused_gen_blocks.c"
    if [ -n "$MSVCRT_AOT_RANGE" ] || [ ! -f "$MSVCRT_GENFILE" ] || [ "$WEBW/x86toc.py" -nt "$MSVCRT_GENFILE" ] || [ "$MSVCRT" -nt "$MSVCRT_GENFILE" ]; then
      echo "  [genblocks] translating msvcrt.dll -> $(basename "$MSVCRT_GENFILE")"
      MSVCRT_RANGE="${MSVCRT_AOT_RANGE:-0x10001000-0x1007c000}"
      MSVCRT_ARGS=( $MSVCRT_RANGE )
      MSVCRT_CFG_ARGS=(--cfg-exports)
      MSVCRT_FP_ARGS=(--no-fp)
      # Focused tables may contain more than one disjoint function.  The
      # explicit divide entry is needed because --cfg-exports follows only
      # exports whose entry lies in the final range; retaining it here makes
      # the focused memset experiment reproducible without weakening the
      # full-DLL CFG filtering.
      if [ -n "$MSVCRT_AOT_RANGE" ]; then
        # Focused experiments can safely use the translator's verified SSE
        # helpers; the broad research table remains integer-only.
        MSVCRT_FP_ARGS=()
        # The explicit ranges are the focused table's complete root set;
        # --cfg-exports would discard non-exported helpers when ranges are
        # disjoint, so retain the range-driven CFG here.
        MSVCRT_CFG_ARGS=()
      fi
      python3 "$WEBW/x86toc.py" "$MSVCRT" "$MSVCRT_GENFILE" \
        "${MSVCRT_ARGS[@]}" --slide-symbol=msvcrt_slide --prefix=msvcrt_ \
        "${MSVCRT_FP_ARGS[@]}" "${MSVCRT_CFG_ARGS[@]}"
    fi
  fi
fi
INC="-Idlls/ntdll -I../dlls/ntdll -I../dlls/ntdll/unix -Iinclude -I../include"
CF2="-D__WINESRC__ -D_NTSYSTEM_ -D_ACRTIMP= -DWINBASEAPI= -DWINE_UNIX_LIB -fvisibility=hidden -fno-stack-protector -fno-strict-aliasing"
emcc $OPT -c "$WEBW/wasm_vm.c"  -o "$OUT/wasm_vm.o"
emcc $OPT -c "$WEBW/wasm_ipc.c" -o "$OUT/wasm_ipc.o"
emcc $OPT -c "$WEBW/wasm_egl_stubs.c" -o "$OUT/wasm_egl_stubs.o"   # EGL entry points emscripten lacks
emcc -std=gnu23 $OPT $CF2 $INC -c "$WEBW/wasm_cpu_bridge.c" -o "$OUT/wasm_cpu_bridge.o"
emcc -std=gnu23 $XOPT $CF2 $INC ${GENBLK:+-DWEBWINE_GENBLOCKS} ${FP_HOT:+-DWEBWINE_FP_HOT} ${PROFILE:+-DWASM_X86_PROFILE} ${MSVCRT_AOT:+-DWEBWINE_MSVCRT_AOT} ${MSVCRT_AOT_RANGE:+-DWEBWINE_MSVCRT_FOCUSED_AOT} -c "$WEBW/wasm_x86.c" -o "$OUT/wasm_x86.o"
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

echo "[4/4] link ($LINKOPT, compile=$OPT, no -g, no ASSERTIONS; 64MB stack for the re-entrant interpreter)"
emcc $LINKOPT @"$RSP" -o "$OUT/webwine.js" \
  --pre-js "$WEBW/browser/node-pre.js" \
  -sNODERAWFS=1 -sENVIRONMENT=node \
  -lEGL -lGLESv2 -sMAX_WEBGL_VERSION=2 -sGL_ENABLE_GET_PROC_ADDRESS=1 \
  -sGLOBAL_BASE=1879048192 -sINITIAL_MEMORY=2147483648 -sALLOW_MEMORY_GROWTH=0 -sSTACK_SIZE=67108864
echo "done -> $OUT/webwine.js"
