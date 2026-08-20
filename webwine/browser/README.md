# netduke32 in the browser — build & performance

Runs the supplied `netduke32.exe` in a Web Worker via native-wasm Wine (Option
B — Wine compiled to wasm, only the app's i386 code emulated by
`../wasm_x86.c`).  MEMFS preload (no NODERAWFS), frames posted to a `<canvas>`.

## Build (run under bash, not zsh)

```sh
export WORK=/tmp/webwine-browser
./build-node.sh            # OPT=-O1: ntdll.so + server objs + interpreter (node harness + shared objects)
./assemble-assets.sh       # ~176MB MEMFS tree: 36-dll import closure + game data + NLS
./build-browser.sh         # -> $WORK/web/webwine-bw.{js,wasm,data} + worker.js + index.html
(cd $WORK/web && python3 -m http.server 8799)   # open http://localhost:8799/
```

Fast measurement loop (no 176MB preload; reads game off the real FS):
`TPUT=1 ./run-node.sh` prints `FPSSAMPLE t=.. flips=.. fps=.. mips=..` once per
wall-second (fps = real `videoNextPage` rate; mips = interpreter throughput).
`HISTO=1 TPUT=1 ./run-node.sh` adds an opcode histogram; `DUMP=1` emits base64
frames on stderr.

## Performance notes (measured this session)

- **`run()` was a 76KB single wasm function — 90% of the whole module.**  That is
  far past the size where the engine's optimizer allocates registers well, and it
  was the *cause* of the old "-O1 beats -O2/-O3" result: higher opt levels inline
  more into an already oversized body.  Splitting the cold blocks out
  (`run_x87()`, `run_sse()` — x87, SSE/MMX, and the bt/cmpxchg/xadd tail, none of
  which appear in the top-16 opcode histogram) cut `run()` to 55KB and **flipped
  the optimum**.
- **Interpreter opt: `-O2`** (`XOPT` in the build scripts).  After the split,
  measured back-to-back both orders: **-O2 ~+11% over -O1**, with -O3 between the
  two.  Before the split -O1 won.  Re-measure this if `run()`'s size changes much.
- **Present at the frame boundary, not on an insn timer.**  `_videoNextPage`
  (VA 0x519620, exe symbol) is the engine's page flip = one real frame.  The old
  build presented every 3M insns, which (a) showed *torn/partial* frames and
  (b) reported a fake ~22 "fps" that was really the capture rate.  Presenting on
  the flip gives clean, complete frames and an honest fps (~8 at 1024×768).
- **`frameplace` is 0 at the flip** (cleared by `videoEndDrawing`), so cache the
  last non-zero `frameplace` pointer during drawing and present *that* buffer at
  the flip — it still holds the finished frame (the pointer is constant across a
  frame's draw).
- **`r_upscalefactor 2`** (autoexec.cfg, preloaded) renders the classic view at
  ½ res and upscales → ~2× fps (8→17 at the menu) for a modest sharpness cost.
  Edit `assemble-assets.sh` / `/game/autoexec.cfg` to set 1 (full res) or 4.
- **Cheaper flags.**  `cond()` used to call `get_flags()`, which materialises all
  six arithmetic flags (parity fold + per-kind switch) to consume one or two bits;
  cmp+jcc is the guest's most common pair.  Per-flag accessors (`lf_zf/sf/pf/cf/of`)
  mirror `get_flags()` exactly and compute only what is asked.  Same for the
  adc/sbb carry-in and the string-op DF test (DF is not even a lazy flag).
- **Frame present runs inside the interpreter loop**, so its cost comes straight
  off the frame rate: build a 256-entry palette LUT once per frame (one indexed
  load per pixel instead of four guest loads + three stores), reuse a grow-only
  buffer instead of malloc/free-ing megabytes every frame, and emit **RGBA** so
  the page wraps the transferred buffer in an `ImageData` directly instead of
  running a per-pixel conversion loop in JS.
- The opcode profile is *flat* (bread-and-butter movs/cmps/movzx/jcc) — no single
  hot handler; cost is per-instruction dispatch.  Real per-frame work is ~10-13M
  guest insns at 1024×768, so resolution is the main lever short of a
  decoded-block JIT (self-modifying code is sparse — ~0.1% of writes land in the
  code region — so a generation-flushed decode cache is viable but is a large,
  correctness-critical refactor).
- **Measurement discipline:** run-to-run noise is ±10% (the attract demo is
  wall-clock locked, so a window lands on different scenes).  Only trust
  back-to-back A/B in one session — `git stash`, rebuild, measure the same
  `[A,B]` window — and prefer `mips` over `fps`.  Things measured and *rejected*:
  pointer-out `decode_modrm` (7% slower — taking `&m` forces the struct to memory),
  `always_inline` on the tiny fetch helpers (no gain at -O1).

## Files

`build-node.sh` `run-node.sh` `build-browser.sh` `assemble-assets.sh`
`dll-closure.py` (PE import BFS) `bw-pre.js` (env + FS symlinks + argv, --pre-js)
`worker.js` `index.html`.  The interpreter's browser hooks live in
`../wasm_x86.c` (present-on-flip, frameplace cache, `WASM_TPUT`/`WASM_HISTO`
diagnostics) and `../wasm_ipc.c` (`WEBWINE_MEMFS` host I/O + `webwine_present`).
