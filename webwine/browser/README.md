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

### The two biggest levers (~20 → ~66 fps)

- **Run at the resolution the renderer is meant for.**  The classic renderer is
  pixel-bound, and the default 1024×768 window costs ~2.5× the pixels for no
  visible benefit.  `autoexec.cfg` (preloaded at `/game`) sets
  `vidmode 640 400 8 0` + `r_upscalefactor 3`, which lands on the engine's
  **320×200 floor**: **~20 → ~52 fps**.  Notes: the engine *refuses* a 320×200
  window and falls back to 640×400 at full res (30 fps), so keeping the upscale
  pass is worth it; `r_upscalefactor` past the floor does nothing; and
  `r_maxfps 0`/`r_vsync 0` confirmed we are render-bound, not capped.
- **Run Wine's own CRT block moves natively — `52 → 66 fps`.**  The guest
  profiler found **69% of ALL guest instructions inside `msvcrt.dll!memmove`'s
  inner copy loop**: the engine blits its framebuffer every frame and we were
  interpreting that byte shuffling one x86 instruction at a time.  msvcrt is
  *our builtin*, not the application, so `memmove`/`memcpy`/`memset` entry points
  are resolved once from the PEB loader list and executed natively
  (`nat_*` in `../wasm_x86.c`).  Dispatch is a direct-mapped table — **one
  indexed load + compare**, the same cost as the frame-flip check it replaced, so
  `run()`'s register pressure does not grow.  It declines (and lets the guest
  code run) if the buffers are not wholly inside the guest address space.
  msvcrt vanished from the profile completely.

### Native fast paths for the engine's hot loops

Eight of netduke32's inner loops now run natively instead of interpreted. They
share one self-validating pattern: the **opcode skeleton is verified
byte-for-byte** with only the engine's patched immediate fields as wildcards, so
a fast path cannot fire on unrelated code, and each has an env kill-switch (which
is also how they get measured).

| loop | what it is | measured |
|---|---|---|
| `msvcrt` memmove/memcpy/memset | Wine's own CRT, resolved from the PEB | 52 → 66 fps |
| `vlineasm4` (0x6321f3) | 4-column texture mapper, ~15,500 iters/frame | +41% |
| `mvlineasm4` (0x6325a0) | **masked** 4-column mapper, ~17,000 iters/frame | +38–50% |
| `mhlineskipmodify` (0x632aa9) | masked **horizontal** mapper (floors/ceilings) | +9.7% |
| `vlineasm1` (0x631cf7) | single-column mapper, 1189 iters/frame | ~1.4% (counter) |
| 8bpp→32bpp span (0x519a41) | the whole `videoNextPage` conversion, 128k px/frame | +50% |
| …then skipped entirely | nothing on our path reads the converted surface | +3.7% |
| libdivide gen (0x401e60) | memoised magic-number generator, 85% hit rate | +15% |
| sprite-timer walk (0x5196c0) | per-frame `videoNextPage` bookkeeping | +10% |

End to end, with every switch above flipped off vs on **in the same build,
interleaved, at matched load** (mips within 7% across all four runs), on the same
scripted walk through E1L1:

| | fps | guest instructions per frame |
|---|---|---|
| all off | 47 | 2048 / 2066 |
| all on | 163 / 155 | 622 / 626 |

**3.4× the frame rate, 70% fewer interpreted instructions per frame.**

**The self-modifying code that blocks a block JIT is what makes this easy.** The
engine patches shift counts, fixed-point steps and every texture/palette/frame
displacement straight into the instruction stream before each call (they read as
`0x88` filler on disk). A JIT would have to recompile constantly; we just re-read
the immediates from the code bytes on entry.

Three techniques worth reusing:

- **Decline and fall through.** A hook that returns 0 from `nat_call` leaves the
  guest to run its own code. That is how `SDL_PollEvent` is intercepted for only
  the events we synthesise, how a fast path bails when its preconditions do not
  hold, and how every kill-switch works.
- **Cache instead of reimplement.** For libdivide the guest computes every value
  we store (a miss runs the real generator through a nested `run()`), so a cached
  divider is by construction the one it would have produced. Reimplementing it
  would have been worth ~3% more and risked silently skewing the renderer's
  perspective maths.
- **Verify against the guest, not against yourself.** Where the output is not
  visible on screen, check it another way: `tests-spanequiv.c` diffs the surface
  conversion's two-loop original against the merged loop over 20000 randomised
  cases, and `WASM_LIBDIV_VERIFY=1` runs the real generator on every cache hit
  (706,772 checked, zero mismatches). Where it *is* visible, a full-canvas pixel
  diff against a run with the fast path disabled is the strongest check —
  `mvlineasm4` and `mhlineskipmodify` are both **0 differing pixels** over
  1022×640.

**No OpenGL here, and the engine does not always believe it.**  `OPENGL32.DLL`
does not load, so every GL entry point is NULL — and applying a video mode still
walks the engine's GL state teardown even with the software renderer.  Two
intercepts keep that from calling through a NULL pointer (which ends the guest
thread and looks exactly like a freeze).  The second is the interesting one: the
engine gates every teardown call on `if (inthash_find(cache, CAP)) glDisable(CAP)`
and there are ~100 such sites, so the fix gates the **lookup** — with no GL
nothing can be enabled, the lookup must miss, and the guest's own branch skips
them all.  Scoped to the 16 GL state tables only; every other `inthash_find` runs
untouched.

**Where it stands now: the profile is genuinely flat.** What is left is real
engine work spread across large functions — `wallscan`, `maskwallscan`,
`classicDrawSprite`, `prepwall` — none of them loop-shaped enough to replace, and
the interpreter's own dispatch is already tuned (the two-byte `0f` opcodes that
matter are all inlined in `run()`). Past this, only a decoded-block cache or JIT
would move the number.

### Profiling the guest

`WASM_PROF=1` turns on an eip sampler that piggybacks on the existing
~64K-instruction housekeeping tick, so **the hot path pays nothing**.  It prints
`PROF <addr> <count> [module!export+off]`, resolving non-exe addresses by walking
the PE headers in guest memory.  Map exe addresses to names with the shipped
symbols (`VA = symbol value + 0x401000`).  This is what found the memmove
result; guessing would not have.  The stride is **jittered** and the sampler is
compile-time gated (`PROFILE=1`), so production builds pay nothing.

It prints `PROF TOTAL <n> dropped <n>` per window, and **percentages must be taken
against that total, not against the listed lines**.  It now dumps *every* occupied
slot rather than a top-60, which matters more than it sounds: computing shares
against a listed subset made every tight loop look dominant, because one loop
fills the whole list while diffuse code (thousands of addresses, a few samples
each) never appears at all.  That reported one loop at 96% of the frame which a
direct counter put at 14%.

**This sampler has now overstated a target twice.**  The second time it put the
audio mixer at 6.2% of the frame where two independent counters both said 1.8%.
The cause was the *jitter* on the sampling stride: the stride is jittered
precisely because a fixed period aliases against periodic work, but the jitter
came from `lcg % 50000` — the LOW bits of an LCG, whose periods are tiny (bit 0
alternates, bit 1 has period 4…).  The "random" stride was itself periodic and
still aliased.  It now takes the high bits, which roughly halved the error on the
one function whose true cost is known exactly.

So: **confirm any profile claim with `WASM_COUNT_ADDR=<hex>` before building
anything on it.**  It counts executions of one guest address; multiply by the
instructions in the loop body and compare against `kinsn/frame`.  Doing that
avoided writing a native audio mixer worth 1.8%, and correctly sized
`mhlineskipmodify` at 4.5% when the sampler claimed 7.2%.

Two more measurement rules learned the hard way here:

- **`kinsn/frame` (from `WASM_TPUT=1`), not fps, is the load-independent metric.**
  Background load on this machine swung 2× mid-session; fps followed it and
  inverted an A/B result. `mips` differing between two arms is the tell that a
  pair is polluted — discard it, don't average it. (One such pair was caused by
  three orphaned test processes of *my own* spinning a core each.)
- **fps is still the thing to report**, because work removed from *native* code
  never shows in `kinsn/frame` at all — skipping the surface conversion is +3.7%
  fps at byte-identical instruction counts.

- **THE governing constraint: `run()` is register-pressure bound.**  It was a
  **76KB single wasm function — 90% of the whole module** — far past the size
  where the engine's optimizer allocates registers well.  Everything follows from
  this:
  - It *caused* the old "-O1 beats -O2/-O3" result (higher opt inlines more into
    an already oversized body).
  - **What works is making `run()` smaller.**  Splitting cold blocks out —
    `run_x87()` (x87), `run_sse()` (SSE/MMX + the bt/cmpxchg/xadd tail),
    `run_cold()` (grp3 mul/div + string ops); none appear in the top-16 opcode
    histogram — took `run()` **76.5KB → 45.7KB** and flipped the opt optimum.
  - **…but check the LINKED module, not the object file.**  `wasm-func-sizes.py`
    measures real code bytes.  `run()` is **79.5KB in the linked wasm** — larger
    than it started — because the C `noinline` attribute does not survive into
    wasm and **Binaryen's own inliner (`emcc -O2` at link time) pulls every
    single-caller function straight back in**.  `-sINLINING_LIMIT` will not help:
    it is documented as LLVM-only, *"does not affect the inlining policy in
    Binaryen."*
  - **Forcing the split back apart is a LOSS — measured, then reverted.**  Calling
    the cold blocks through `volatile` function pointers is something no inliner
    can see through, and it does work: `run()` **79.5KB → 49.1KB**, with
    `run_sse` 20.1KB, `run_x87` 7.6KB, `run_cold` 8.7KB standing on their own.
    It was **~8% slower** (mips 101.7 vs 110.1 on the one interleaved pair whose
    load matched).  The reason is that the premise was wrong: `run_cold` also
    handles **shifts**, and `c1` is 217M in the opcode histogram, so it is not
    cold at all and every shift paid for an indirect call.  Size is a proxy, not
    the goal — do not chase it without measuring.
  - **What does NOT work is anything that adds live values**, even when it removes
    real memory traffic.  Measured and rejected: pointer-out `decode_modrm`
    (**−7%**), a packed-u64 modrm return that made both decode structs pure wasm
    locals and halved the loads/stores (**−5%**, verified correct by a 4M-case
    differential test), mirroring `g_flip_addr`/`g_histo_on` into locals
    (**−2.8%**), branchless `sizemask`/`signmask` (**−1.3%**), `always_inline` on
    the tiny fetch helpers (no gain).
  Shadow-stack accesses here are constant-address L1 hits with store-to-load
  forwarding — effectively free — so trading them for register pressure loses.
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
- **Resolution is set in `autoexec.cfg`** (preloaded at `/game`): `vidmode 640 400
  8 0` + `r_upscalefactor 3` lands on the engine's 320×200 floor.  See the two
  big levers above; edit `assemble-assets.sh` to change it.
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
- **Diagnostic counter off the hot path:** the guest instruction count was a 64-bit
  *global* bumped every instruction (i64 load + i64 store to linear memory).  It
  now lives in a local and is folded into the global at the tick and at the
  `RUN_RETURN` sites; accumulating a *delta* rather than mirroring the total keeps
  it correct when `run()` re-enters itself for user callbacks.
- **`rep` string ops are NOT worth bulk-memcpy'ing** (checked by disassembling the
  exe): 596 `rep` sites, but every per-frame one has a small compile-time-constant
  `ECX` — these are inlined struct copies, not framebuffer blits.
- **OffscreenCanvas does NOT work here** — don't retry it.  Transferring the canvas
  to the worker so `webwine_present()` can `putImageData` directly (saving a
  full-framebuffer alloc + copy + transfer per frame) *runs* — fps and the guest
  log look perfectly normal — but **the canvas stays black**: an OffscreenCanvas
  only composites when its task yields, and this worker never returns to its event
  loop (it is blocked inside the interpreter's `run()` for the whole session).  The
  main thread has to do the drawing, so the postMessage path is required.  Note the
  verification trap: `canvas.toDataURL()` returns blank for a transferred canvas,
  so use CDP `Page.captureScreenshot` (see `cdp-shot.js`) to check real output.
- **Measurement discipline:** run-to-run noise is ±10% (the attract demo is
  wall-clock locked, so a window lands on different scenes), and background CPU
  load shifts absolute numbers a lot.  Only trust back-to-back A/B in one session
  — `git stash`, rebuild, measure the same `[A,B]` window, ideally in both orders
  — and prefer `mips` over `fps`.  `relink_x86.sh`-style rebuilds of only
  `wasm_x86.o` isolate the interpreter and are much faster than a full rebuild.


## Input

Keyboard and mouse are delivered from the page through a SharedArrayBuffer ring
(the worker never returns to its event loop, so postMessage cannot reach it).
`wasm_drain_browser_input()` at the top of `NtUserPeekMessage` turns ring records
into window messages: 1=keydown 2=keyup 3=absolute move 4/5=button down/up
6=relative motion.

**Mouselook needs pointer lock and a synthetic SDL event — window messages are
not enough.** Two facts force this:

- Only under **pointer lock** does the browser report unbounded
  `movementX/movementY`; unlocked, the cursor stops at the window edge and turning
  stops with it. Clicking the canvas locks, Esc releases, and the status line
  says which state you are in.
- The game **never polls mouse state** — `SDL_GetRelativeMouseState` has *zero*
  call sites in the exe. It calls `SDL_SetRelativeMouseMode` and then reads
  `xrel`/`yrel` out of **`SDL_MOUSEMOTION` events**. Real SDL answers relative
  mode by switching its Windows backend to **raw input (`WM_INPUT`)**, which we do
  not deliver — so no amount of `WM_MOUSEMOVE` will ever drive mouselook.

So `SDL_SetRelativeMouseMode` is accepted (returns success; the game hides its
cursor and switches to `xrel`/`yrel`) and `SDL_PollEvent` is intercepted to inject
a synthetic `SDL_MouseMotionEvent` built from the pointer-lock deltas. The
interception is **partial on purpose**: with no delta pending the hook returns 0
from `nat_call`, so the *real* `SDL_PollEvent` runs and keys, buttons and quit are
untouched. (That "decline and fall through" is the general recipe for hooking one
case of a function.) The struct layout came from the exe's own **DWARF** — it
ships `.debug_info` — not from guessing: 36 bytes, `type@0 timestamp@4
windowID@8 which@12 state@16 x@20 y@24 xrel@28 yrel@32`, `SDL_MOUSEMOTION` 1024.
`WASM_NO_MOUSE=1` disables the whole path.

**Aspect ratio.** The canvas display size is computed from the frame's real
dimensions with one scale factor on both axes, fitted into a 1024x768 box — 320x200
renders as 1024x640. It was previously hard-coded 4:3 against an 8:5 framebuffer,
i.e. stretched vertically; the status line now prints the ratio so a regression is
visible.

**JS trap, hit twice:** `noteInput()` runs before the page finishes evaluating, so
any `let` it touches must be declared *above* it. A temporal-dead-zone throw kills
the whole page script silently — blank page, empty log, zero frames, and nothing
that points at input.


## Audio

Sound effects work; **music is off on purpose** (see below).

SDL could never open a device here, and the driver was never the problem:
`SDL_OpenAudioDevice` starts an audio **thread**, and the interpreter has a single
guest CPU. So we take SDL's place - its audio entry points are intercepted (after
byte-checking each 6-byte dynapi thunk) and we call the game's own audio callback
ourselves, shipping PCM to the page through a second SharedArrayBuffer ring that
an `AudioWorklet` drains.

What keeps it smooth, and correct:
- The callback only runs while the game is **not** holding the SDL audio lock
  (`SDL_LockAudioDevice`/`Unlock` are intercepted to track depth) - the same
  contract SDL gives the mixer, which matters because we are single-threaded.
- It is pumped **at the frame flip**, not on the arbitrary housekeeping tick. An
  arbitrary instruction boundary can land inside the guest's own heap/CRT locks,
  and guest critical sections are recursive for one thread, so a mixer `malloc`
  could re-enter a half-updated allocator. The flip is a clean function entry,
  and at ~84 fps it comfortably outruns the ~43 buffers/sec needed.
- Each visit tops the queue up to a ~140ms cushion and no further: every queued
  frame is interpreted guest code, so rendering ahead steals CPU from the renderer.
- On underrun the worklet emits **silence and re-cushions** rather than repeating
  the last block (a repeat loops a short period into an audible buzz, and since it
  consumes no ring data it is self-perpetuating).

### Why music is off — measured, not assumed

`mus_enabled 0` in `autoexec.cfg`. With music on, the guest profile is **100%
`AdLibDrv_MIDI_Service`** — the Nuked OPL3 synthesiser, LTO-inlined into it:

| | audio cost | callbacks/sec | result |
|---|---|---|---|
| SFX only | ~1.4-2.8 M insn/s (**~3%**) | 43-44 (full rate) | 80+ fps, no underrun |
| + OPL3 music | ~165-180 M insn/s (**~98%**) | 20 of 43 needed | ~2 fps, permanent underrun |

Music needs ~357M guest instructions/sec against an interpreter that does ~110M/s
— about **3x the entire budget** — so it is not a tuning problem. Lowering the
rate does not help (the game already asks for 22050) and the mitigations bottom
out. Real music would need a *native* OPL3 core driven by intercepting the game's
register writes, or a MIDI-to-WebAudio path; both are separate projects.
To hear it anyway, set `mus_enabled 1` and expect single-digit fps.

## Files

`build-node.sh` `run-node.sh` `build-browser.sh` `assemble-assets.sh`
`dll-closure.py` (PE import BFS) `bw-pre.js` (env + FS symlinks + argv, --pre-js)
`worker.js` `index.html`.  The interpreter's browser hooks live in
`../wasm_x86.c` (present-on-flip, frameplace cache, `WASM_TPUT`/`WASM_HISTO`
diagnostics) and `../wasm_ipc.c` (`WEBWINE_MEMFS` host I/O + `webwine_present`).
