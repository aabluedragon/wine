# Current browser checkpoint

## 2026-08-18: click-jump fix (Pointer Lock acquire spike + drifted click pos)

After relative mouselook worked, clicking snapped the aim. Two causes, both
fixed:
- **Acquire spike:** the first browser `movementX/Y` after Pointer Lock is
  granted (which happens on the locking click) is the huge jump to the lock
  centre. `knativeinputSDL`: on the transition into relative mode, arm an
  8-frame settle window that drops deltas > 150 px; a permanent 1000 px clamp
  also catches any later glitch spike, both well above real flicks.
- **Drifted click position:** under Pointer Lock the synthetic absolute
  position drifts far off-screen as you turn; delivering it with a button
  press jumped the guest pointer. `xserver.cpp mouseButton` now feeds a
  neutral (0,0) position for grabbed + XI-raw-motion clicks under emscripten
  (mirrors the existing `forceRelativeMouse` button handling, minus the warp).

Note: `ipconfig getifaddr en0` can report a **VPN** address (utun*) instead
of the Wi-Fi LAN IP — check `ifconfig | grep 'inet '` for the real
192.168.x.x before generating the cert / handing out the URL.

## 2026-08-18: mouse aim fix (relative/Pointer Lock) + LAN HTTPS serving

**Crazy FPS aim — root-caused by instrumentation, then fixed.** Added
`[MOUSEDBG]` logging to the mouse path and drove it with cdp-run's new
`--mouselook`. Findings: netduke32 under Wine grabs the pointer and selects
**XInput2 raw motion** (`grabbed=1 rawMotion=1`), but BoxedWine fed Wine the
**absolute** cursor position, read as a raw delta → spin. My first attempt
(tie relative mode to cursor-hide in setCursor) failed: the cursor is set
several times at boot (a cached *visible* cursor among them) and the spurious
"visible" event turned relative mode back off; also `SDL_SetRelativeMouseMode`
at boot silently fails (Pointer Lock needs a user gesture). Confirmed via the
log: `relMode=0`, `xrel=0`.

Working design (grab-driven, gesture-correct):
- `source/x11/xserver.cpp`: on every mouseMove, set `Module.boxedwineCaptureMouse`
  from the reliable **grab + XI raw-motion** state (the real "wants mouselook"
  signal), and when called with `relative=true` deliver the delta straight to
  the grabbed window as XI2 raw motion (no warp — warp is a browser no-op
  without Pointer Lock). `emscripten.h` included under `__EMSCRIPTEN__`.
- `platform/sdl/knativescreenSDL.cpp`: export `boxedwine_set_relative_mouse(int)`
  (EMSCRIPTEN_KEEPALIVE) so the shell can enter/leave SDL relative mode from
  inside a click gesture (the only context where the browser grants Pointer
  Lock). The old cursor-hide relative toggle was removed.
- `platform/sdl/knativeinputSDL.cpp`: under `SDL_GetRelativeMouseMode()`,
  deliver `e->motion.xrel/yrel` (real movementX/Y) instead of absolute x/y,
  scaled by the display→guest ratio, no offset.
- makefile: `_boxedwine_set_relative_mouse` added to jitControlGL exports.
- Shell (`boxedwine.html` checkbox default `checked`; `boxedwine-shell.js`):
  on canvas click, if `Module.boxedwineCaptureMouse`, call
  `ccall('boxedwine_set_relative_mouse',...,[1])` — engages SDL relative mode +
  Pointer Lock inside the gesture; on `pointerlockchange` to unlocked, call it
  with 0 (menus / Escape return to absolute).

Verified links (headless/headful): grab+rawMotion detected; the click ccall
flips `relMode` 0→1. The final `movementX → xrel → delivery` link can only be
exercised with a **physical mouse** — synthetic CDP/headless input cannot
produce `movementX` (Pointer Lock is refused headless: `WrongDocumentError`),
so `xrel` stays 0 in automation regardless. Needs a hands-on confirm.

**LAN testing (phones/tablets):** `serve-https.mjs` serves over HTTPS with a
self-signed cert — required because the threaded WASM build needs
`crossOriginIsolated` (SharedArrayBuffer), which browsers grant only in a
secure context; `http://<LAN-IP>` is not one. Run:
`PORT=8443 CERT=<cert> KEY=<key> node serve-https.mjs build-jitgl`. Generate
the cert with the **current** LAN IP in the SAN (DHCP can change it — re-gen
and restart if the phone can't connect). cdp-run gained
`--ignore-certificate-errors` and `--headful`. Verified: game renders in-level
over `https://<LAN-IP>:8443` with the Pointer-Lock checkbox pre-checked.

## 2026-08-18 latest: paced flushes validated head-to-head

Interleaved A/B (paced on 8093 vs unpaced flag-build `BOXEDWINE_WASM_JIT_-
UNPACED_FLUSH` on 8094, same URL, alternating 4-min runs): the unpaced
build hit a **2639 ms** freeze in round 1; the paced build's worst pause
across both rounds was **150 ms**. Matched-contention round 2 shows fps
parity (53.6 vs 52.2 mean) — pacing costs nothing at steady state because
the flush queue is empty once warm. Paced stays the production build on
8093; the unpaced comparison build is served on 8094. Note the zsh trap
that ate the first A/B attempt: `set -- $var` does not word-split in zsh.

## 2026-08-18 night II: stutter hardening — paced JIT flushes, cache verdict

Three further anti-stutter results on the JIT runtime (port 8093):

1. **20-min soak**: after the stored-zip fix, all gameplay stalls were JIT
   warmup in the first ~3 min (564/260/221 ms, decaying), then zero stalls
   for 15 straight minutes.
2. **Persistent JIT cache is a stutter source, confirmed with a paired
   run** (new `--profile-dir` in cdp-run keeps IndexedDB): the warm second
   session hit a **2.5 s stall** and lower fps. `jit-cache=off` must stay
   in every URL.
3. **Paced batch flushes** (BoxedWine edits: `queueRuntimeFlushes` now
   compiles at most one batch per call; `wasmJitDrainSealedRequests()`
   drains one batch per browser tick from the emscripten mainloop):
   warmup stalls drop to **worst 121–275 ms even under external CPU
   contention** (was 337–927 ms). Guest threads that reach a still-pending
   block still take the urgent pending-hit flush, so warmup coverage keeps
   up. Deployed on 8093.


## 2026-08-18 late: seconds-long stutters fixed — store the GRP uncompressed

The user-reported multi-second freezes "in some places" were BoxedWine's
zip FS re-inflating the deflated 44 MB DUKE3D.GRP on in-game asset reads
(lazy tile/sound loads when entering new areas): `unzReadCurrentFile`
dominated CPU-profile buckets mid-walk, and `fszipopennode.cpp` shows
stored entries (`compressionMethod == 0`) get a direct lseek+read path
while deflated entries re-run `setupZipRead`. Fix with no code change:
**`netduke32-up3m250s.zip`** = same package re-zipped with `zip -0`
(store); `netduke32-up3m250.zip` and `netduke32-up3m.zip` were then replaced
with stored repacks too, so every published URL carries the fix. Scripted-walk A/B (new `--hold key@from:to` in cdp-run):
49 stalls (worst 591 ms) → **4 stalls (worst 337 ms)**, walk-phase fps
70.9 → 95.4 mean. Final validation: a 345 s combat walk (movement + firing
+ doors, `--light`, quiet machine) shows **zero** main-loop stalls, zero
sub-30 fps seconds, 134.6 fps mean / 68 min. Root-zip repack was attempted for boot speed and
abandoned (unzip/zip roundtrip breaks `.link` symlink entries).

Instrumentation honesty: cdp-run's per-second `Page.captureScreenshot`
sampling is itself a multi-second `readPixels` stall under SwiftShader —
the 4.3 s "stall" in the first stored-zip run was harness-induced. New
`--light` flag disables readbacks; use it for any pacing measurement.

## 2026-08-18 late night: main-loop timing fix — **126 fps median**

A V8 CPU profile (new `--cpuprofile start:dur` option in cdp-run.mjs)
showed **27.9% idle wall-time**: BoxedWine's emscripten mainloop used
`EM_TIMING_SETTIMEOUT, 1`, and Chrome clamps nested setTimeout to ~4 ms.
Switching to `EM_TIMING_SETIMMEDIATE, 0` (postMessage, unclamped — edit in
`source/sdl/emscripten/mainloop.cpp`, jitControlGL relinked with
`--profiling-funcs` for named wasm frames) measures **124.7 mean /
126 median / 106 min** clean on the same up3m250 URL (port 8093).
Contended runs mislead badly here: the same build read 75 with the user's
native test running — never trust a number without checking `ps aux -r`.
Named profile shows the next ceiling: ~40% interpreter dispatch
(NormalCPU::run/normalDispatch/normal_*) even with the JIT on, plus 11.6%
wasmStartJITOp — JIT block coverage/entry cost is the next target.

**Compile-threshold experiments (conclusive negatives):** guest execution
splits 52.4% interpreter / 24.7% JIT-generated / 11.6% JIT entry gate.
`JIT_RUN_COUNT` (blocks compile after N executions, default 200, overridable
via `GCC_EXTRA_FLAGS=-DJIT_RUN_COUNT=N`) is well-tuned: N=100 measures
**81.9 clean** (compile churn of cold blocks costs more than the coverage
gains) and N=20 **crashes the emulator during boot** (memory access out of
bounds — latent bug with eager compilation during process startup). The
default-200 build is restored and redeployed on 8093. Raising JIT coverage
needs a different mechanism (e.g. trace-based selection or cheaper entry),
not a lower threshold.

## 2026-08-18 night, clean re-measure: **115 fps median**

On a quiet machine, `netduke32-up3m250.zip` on the JIT runtime (port 8093,
`&audioFreq=22050&jit-cache=off`) measures **113.5 fps mean / 115 median /
99 min / 131 max** over the last 60 s of gameplay — the 81 below was
contention-depressed. Factor 4 + uncap is *slower* (99.9) — the extra
software-upscale pass outweighs the raster saving once uncapped; factor 3
wins on both axes. Also: the "warm cache pathology" attribution for today's
slow runs was wrong — those runs compiled all 48k blocks fresh; the
variance was external CPU load (the user's native tests) — but keep
`jit-cache=off` anyway. Gameplay screenshot verified; MIPS ~270 sustained.

## 2026-08-18 night: JIT runtime + uncapped frame limit — 81 fps median

The sibling-edit constraint was lifted (goal re-issued without it), enabling a
new BoxedWine make target `jitControlGL` (WASM JIT + FULL_ES2, no indexed
renderer — the makefile edit is in the BoxedWine tree). Deployed at
`web-showcase/build-jitgl`, served on port **8093**. Findings:

- The JIT lifts guest throughput to **404–437 MIPS** (vs ~170 interpreted),
  but fps stayed ~70: the game's own frame limiter was the ceiling all along.
- `netduke32-up3m250.zip` adds `r_maxfps 250` to the autoexec. On the JIT
  runtime with `&jit-cache=off&audioFreq=22050`: **81.6 mean / 81 median**,
  measured *under* heavy external CPU contention — clean numbers should be
  higher. Compile stalls cause occasional hitches (min 40, "ran main loop in
  940ms").
- `jit-cache=off` is required: the persisted module cache makes warm runs
  *slower* (130 MIPS vs 404 cold — the known re-install pathology).
- On the interpreter, `r_maxfps 250` gains nothing (CPU-bound at ~170 MIPS);
  the interpreter URL (port 8089, up3m, 71.7 fps) remains the stable
  fallback with no hitches.

Last verified: **2026-08-18 (morning, +03:00)** — the verified GL config
re-measured **34–35 fps** (last-60s window; samples 32–36) on the freshly
rebuilt clean `controlGL` runtime in `build-ctlgl2` (port 8089), screenshot
showing in-game E1L1 with HUD after Space input. Same URL as the 28.9 fps
entry below; the improvement is machine-load variance plus the clean rebuild.

## 2026-08-18 final: 71.7 fps; the plateau and what's left

Adding `&audioFreq=22050` to the URL (host SDL audio matched to the guest's
22 kHz mix rate) measures 71.7 mean / 71.5 median — a marginal consistent
gain; host `sound=false` is no better (71.1) and unstable (min 53). This is
the plateau for config-level work: the remaining ~14 ms frame is emulated
game logic + Wine plumbing at ~170 MIPS. The persistent MIPS hotspot (libc
memcpy, ret at `ntdll.so+0x8c000`) is not a code address — 0x8c000 is
.got.plt in the packaged ntdll — i.e. the syscall-dispatch gate; finding
the actual hot syscall needs emulator-side instrumentation (out of scope
under the no-sibling-edit constraint). Day's ladder: 24.6 → 45.6 → 63 →
70.6 → 71.7 fps.

Also ruled out (measured): `cpu=p3` URL param = 68.2 (PIII feature level is
no better — SIMD paths cost more emulated than they save);
`WINEDEBUG=+server` profiling through the console is unusable (the flood
throttles boot so hard the game never starts in 420 s), and the boot-era
trace shows only registry churn — wineserver is not the gameplay
bottleneck.

## 2026-08-18 evening: 70 fps with sound and factor-3 visuals (up3m)

`netduke32-up3m.zip` (autoexec: `r_upscalefactor 3`, `snd_mixrate 22050`,
`snd_numvoices 16`) sustains **70.6 fps mean / 70 median / 68 min** — audio
mixing at 44.1 kHz / 96 voices was costing ~1.7 ms/frame, the entire gap
between the factor-3 (63) and factor-4 (70) tiers. This is the recommended
default: factor-3 sharpness, sound retained at 22 kHz. Also measured:
factor 6 = 70.6 (upscale clamps at the factor-4 floor, no further gain);
ScreenSize=8 = 68 mean but min 52 and a smaller view — rejected. Curiously
`-ns -nm` (sound fully off) measured *slower* — reduce the mix cost, don't
remove the mixer.

## 2026-08-18 later: upscale tiers — 63 fps at factor 3, 70 at factor 4

`r_upscalefactor` scales further (solo serial runs, same conditions,
last-60 s gameplay means): factor 2 = 40.9–45.6, **factor 3 = 63.1 (median
63, min 51)**, factor 4 = 70.4 (median 70, min 65). Factor 3 (~213x133
internal 3D view) is visually near-identical to factor 2 in screenshots and
is the recommended default: `app=netduke32-up3.zip`. Factor 4 (160x100) is
the speed tier: `netduke32-up4.zip`. All keep the 320x200 glsurface upload
(the engine software-doubles the low-res view into it), so draws/s equals
uploads/s — the fps metric stays valid. `-ns -nm` (no sound) measured
*slower* (33 vs 41 control) — not a lever.

## 2026-08-18 afternoon: r_upscalefactor 2 — 45.6 fps mean (1.85x)

The game ignores the cfg's 320x240 (its own log shows `Setting video mode
640x400 (8-bpp windowed)`; OSD keystrokes never reach it), but it executes
`autoexec.cfg` at startup. `netduke32-up2.zip` = the verified package plus an
`autoexec.cfg` containing `r_upscalefactor 2`: the classic renderer then
rasterizes 320x200 internally and the GPU upscales (glsurface texture drops
from 640x400 to 320x200). Back-to-back A/B under identical machine load:
**baseline 24.6 fps mean vs 45.6 mean / 43 median / 66 peak** over the last
60 s of gameplay. Same URL as below with `app=netduke32-up2.zip`.

Measurement discipline learned today: fps runs must be **solo** — a concurrent
second emulator, the log-dump bat loop, the user's native processes, or macOS
`mediaanalysisd` bursts each halve the number (the 12 fps "regression" and a
9 fps reading were pure contention). Diagnostics that worked: a run.bat that
`start`s the game then periodically `type`s `netduke32.log` to the console
(cmd for-loops run ~2k iterations/sec emulated — keep loops ≤20k, `@echo off`
mandatory); `--readfile` via emscripten FS does NOT reach the guest disk.

## 2026-08-18: Polymost 32-bpp status; fps work

Polymost (ScreenBPP=32, `netduke32-gl32.zip`) now runs **crash-free** on the
`controlLegacyGL` build (`web-showcase/build-legacy2`, port 8091) — 230 s,
~473k draws, no exception — but renders **black**. The chain that got it
crash-free, all inside this repo (no sibling-source edits since the constraint):

1. `tools/patch-boxedwine-legacy-gl-imports.mjs` (applied to the deployed
   `build-legacy2/boxedwine.js`): the wasm import object snapshots
   `_emscripten_gl*` before LEGACY_GL_EMULATION wraps them, so all guest GL
   bypassed the emulation layer. The patch late-binds all 246 entries,
   adds draw-time bookkeeping repair (`__bwEnsureGLBookkeeping`), and makes
   `glDetachShader`/`glDeleteShader` record-preserving (the game detaches and
   deletes shaders after link; the emulation's wrappers erased the records
   `Renderer.init` needs).
2. Page-level GLSL ES repair in `tools/cdp-run.mjs` FRAME_HOOK: libglemu
   prepends declarations before the precision header and after `#extension`
   lines — hoist `#extension`, prepend guarded `precision highp float`. All 21
   shader compiles then pass.

Remaining black-output fault: draws land on the default framebuffer, viewport
640x480, program bound, all three samplers verified valid at draw time
(tile LUMINANCE, palswap 256x32 LUMINANCE, palette 256x1 RGBA) — readPixels is
uniformly (0,0,0,255). Next unexamined link: libglemu's user-program
vertex-attribute delivery at flush.

**Strategic finding:** Polymost through int99 dispatch sustains only
~2,400–3,500 draws/sec → a 5–10 fps ceiling at its 300–800 draws/frame. The
glsurface path (1 upload + 1 draw/frame) is the high-fps route. Measured
legacy-GL tax on the same glsurface config: 14.9 fps (controlLegacyGL) vs 28.9
(controlGL). A 320x240-desktop variant (`netduke32-sw-320d.zip`) measured
12.07 fps on the freshly rebuilt ControlGL, while the verified 640x480-desktop
config re-measured 34–35 fps on the same build — the 320x240-desktop variant is
a dead end, not a build regression.

## OPENGL WORKS END TO END (2026-08-17 evening)

The supplied `netduke32.exe` (`547dea93…33878`) renders its OpenGL path in the
browser with working keyboard input at **28.9 fps mean / 29 median** (640x400
GL surface, 320x240 game resolution config). Screenshots show E1L1 rendered
through the palette shader with correct colours and HUD; the view changes after
w/a/d input.

Serve `web-showcase/build-ctlgl2` (`controlLegacyGL`-era `controlGL` BoxedWine
build, `boxedwine.wasm` SHA `dc2dcfdb…`) on port 8089 and open:

```text
http://localhost:8089/boxedwine.html?root=tinycore-wine11-glxshim-browserboot-noerror.zip&app=netduke32-sw-320.zip&resolution=640x480&storage=memory&w=/home/username/.wine/dosdevices/c:/files/netduke32&p=run.bat&args=-cfg%20netduke32.cfg%20-nosetup%20-g%20DUKE3D.GRP%20-v1%20-l1%20-s3&env=%22WINEDLLOVERRIDES:mscoree,mshtml=%22
```

The complete fix chain (Wine-repo scripts + sibling BoxedWine changes, all under
explicit user authorisation for the performance/OpenGL task):

1. `wineboot`/`services` browser stubs with builtins removed (make-browser-root.mjs).
2. `WGL_CONTEXT_OPENGL_NO_ERROR_ARB` dropped from packaged winex11 (patch script).
3. BoxedWine: single browser GL window reused and resized per drawable
   (was: every SDL_CreateWindow rebound the canvas; Wine's 1x1/10x10 GLX probe
   drawables left it 10x10).
4. BoxedWine: `GL_ARB_sampler_objects` advertised; NULL host GL pointers are
   skip-and-log instead of jump-to-0 (glBindSampler was NULL and the game calls
   it unconditionally: eip=0 err=17, retAddr in netduke32.exe).
5. BoxedWine: `GL_RED`/`GL_R8` mapped to `GL_LUMINANCE` for WebGL 1 uploads.
6. BoxedWine: desktop `#version 110` stripped from guest shaders for WebGL
   (GLSL ES), with guarded float precision; shader compile/link failures now log
   the driver's info log.
7. BoxedWine: `KThread::runSignal` re-entrancy guard (fault during signal
   delivery killed the emulator with stack exhaustion; now kills the process).
8. **The black-frame root cause:** SDL's renderer presents the X11 screen
   through the same WebGL context the guest renders with, rebinding its own
   program and textures every frame. Wine's GLX probes also created windows.
   Fixed by gating `drawRect`/`present` on `visible && !glWindowOwnsDisplay()`,
   where the latter is `shownGlWindows > 0 || guestOwnsGlContext` — the flag set
   when the guest makes a GL context current. Confirmed by draw-state probes:
   sampler units went from `34,32 -> 32,32` (clobbered) to a stable `34,35`.

Measured GL vs software on the same runtime: 28.9 fps GL vs 10.1 fps software.

Caveat for productisation: while a guest GL context is current, the SDL screen
presentation is fully suppressed, so the Wine desktop chrome is not drawn.
Correct for a fullscreen GL game; needs state save/restore around SDL_RenderCopy
if desktop compositing and GL are ever wanted simultaneously.

## 2026-08-17: the supplied PE runs and renders in the browser

The supplied executable now boots, renders sustained frames, and responds to
keyboard input in headless Chrome. It runs through its 8-bpp presentation path,
not the 32-bpp OpenGL path that the sections below track; that objective is
still open and its blocker is unchanged.

### Identity

`/Users/alonamir/dev/wine` at `9a57a7d913a0424e09660d5284669132299db6f4`,
branch `vibe`. The working tree is dirty and carries large untracked build
directories; preserve them. Only files under this repository were edited.

The executable is the supplied `/Users/alonamir/games/netduke32_v1.2.1/netduke32.exe`,
SHA-256 `547dea93d40114dee7757a049f20e0f7659cbd0c221ae9cf4258338e94c33878`,
re-verified for this run. Nothing was rebuilt or substituted.

| Artifact | SHA-256 |
| --- | --- |
| `web-showcase/build-gl/netduke32-sw.zip` | `c5b849432de5da22890dd5fe129bee63131b7f8514c4589ea22a62fe80cda430` |
| `web-showcase/build-gl/tinycore-wine11-glxshim-browserboot.zip` | `49576ad784dc773c2ae8a8d92599a93e1046d8a4ac3a7fe118779361ad364e07` |
| `web-showcase/build-gl/tinycore-wine11-glxshim-browserboot-noerror.zip` | `a47512f0b5c5e6b94239ff0d1e1a6fdb6dcc9281adb9a606861902085c520931` |
| `web-showcase/tools/make-browser-root.mjs` | `1ec468003f34fd41ff9ec2af23f627d07f420b8317d4c2811b827033302696eb` |
| `web-showcase/tools/patch-wine11-no-error-attrib.mjs` | `c24f9d4ba79a8dc699b39759a4602b29e7ca6868a4f4298f5ba5c3bba76a2c75` |
| `web-showcase/tools/cdp-run.mjs` | `d0aa3beecd66777abca7a14f5fa2cc191101f2bb6d965606052417e96c06e08f` |

Both roots derive from `tinycore-wine11-parent-inline-webgl-pci-glxshim.zip`
(`47e1d4d0…`). The runtime is the `build-gl` interpreter Control build; the
`build` JIT runtime does not start the guest with this root at all.

### Reproduce

```sh
cd /Users/alonamir/dev/wine/web-showcase
node tools/make-browser-root.mjs \
  build-gl/tinycore-wine11-parent-inline-webgl-pci-glxshim.zip \
  build-gl/tinycore-wine11-glxshim-browserboot.zip
node tools/patch-wine11-no-error-attrib.mjs \
  build-gl/tinycore-wine11-glxshim-browserboot.zip \
  build-gl/tinycore-wine11-glxshim-browserboot-noerror.zip
node serve_gl.mjs build-gl
```

Exact URL (port 8082):

```text
http://localhost:8082/boxedwine.html?root=tinycore-wine11-glxshim-browserboot-noerror.zip&app=netduke32-sw.zip&resolution=640x480&storage=memory&w=/home/username/.wine/dosdevices/c:/files/netduke32&p=run.bat&args=-cfg%20netduke32.cfg%20-nosetup%20-g%20DUKE3D.GRP%20-v1%20-l1%20-s3&env=%22WINEDLLOVERRIDES:mscoree,mshtml,opengl32=%22
```

### The three Wine-side changes that were needed

1. **Bootstrap.** `wineboot`/`services` were replaced by the browser stubs.
   The prefix copies alone had no effect, because a module in the system
   directory loads builtin-first; `make-browser-root.mjs` now also drops
   `opt/wine/lib/wine/i386-{windows,unix}/{wineboot,services}.exe{,.so}` so the
   native stub is the only candidate. Before this the guest deadlocked:

   ```text
   009c:err:sync:RtlpWaitForCriticalSection section 10B453C0
     "dlls/ntdll/loader.c: loader_section" wait timed out in thread 009c,
     blocked by 0094, retrying (60 sec)
   003c:err:service:process_send_command receiving command result timed out
   ```

2. **`WGL_CONTEXT_OPENGL_NO_ERROR_ARB`.** `dlls/winex11.drv/opengl.c` forwards
   this token to GLX without checking for `GLX_ARB_create_context_no_error`.
   BoxedWine's GLX shim rejects it and the emulator then traps:

   ```text
   gl_common_XCreateContextAttribsARB unhandled attribute 31b3
   Uncaught RuntimeError: memory access out of bounds
   ```

   `patch-wine11-no-error-attrib.mjs` applies the equivalent of deleting that
   switch case to the prebuilt i386 module. Wine then logs the harmless
   `err:wgl:x11drv_context_create Unhandled attribList pair: 0x31b3 0` and
   context creation succeeds.

3. **`opengl32` disabled.** With GL available, NetDuke32 loads glad against
   BoxedWine's WebGL, which has no desktop-GL entry points, logs
   `Failed to initialize OpenGL loader!`, and dies on a NULL pointer
   (`Caught signal: SIGSEGV`). Overriding `opengl32=` makes
   `SDL_GL_CreateContext` fail cleanly, so the game takes its 8-bpp path:

   ```text
   79.2220s  GFX| Setting video mode 640x480 (8-bpp windowed).
   61.6410s  ASS| Initialized sound at 44.1 KHz stereo with 96 voices
   ```

### Measured result

`tools/cdp-run.mjs` run of 210 s, keys sent after clicking the canvas:
**193 of 210 samples non-black, 72 distinct frames**, canvas internally
640x480 for the whole run, no exception and no teardown. Screenshots show
E1L1's rooftop with a correct HUD (health 100, armour 0, ammo 48), and the
view changes after `w`/`a`/`d`, so input reaches the game.

Sampling had to change to prove this. The old probe copied the canvas with
`drawImage`; the emulator presents through a WebGL context without
`preserveDrawingBuffer`, so every sample read as pure black while the page was
visibly rendering. `cdp-run.mjs` now samples `Page.captureScreenshot` over the
canvas rectangle. **Earlier "black canvas" conclusions taken with the old probe
are unreliable and should be re-measured before being trusted.**

### Still open

The 32-bpp OpenGL objective below is unchanged: BoxedWine's WebGL backend has
no fixed-function/legacy desktop GL, and that code is in a sibling repository
that must not be modified without explicit authorisation.

## 2026-08-17: performance work

Measured with `tools/cdp-run.mjs`, which now counts presented frames by hooking
WebGL texture uploads on the emulator canvas (`--fps` output). Every figure
below is the mean over the last ~50 s of a run that is in-game, not at a menu.

| Configuration | fps | MIPS | Verdict |
| --- | --- | --- | --- |
| 640x480, interpreter (`Control`) | **10.3** | 171 | baseline |
| 320x240, interpreter | **19.9** | 171 | **adopted, 1.93x** |
| 320x240, sound disabled (`-ns -nm`) | 19.8 | — | no effect, rejected |
| 320x240, desktop+screen also 320x240 | 19.1 | 174 | no effect, rejected |
| 320x240, `bpp=8` | 11.2 | 173 | worse, rejected |
| 320x240, WASM JIT, cold | 15.1 | 236 | worse, rejected |
| 320x240, WASM JIT, 99.9% warm cache | 13.9 | 222 | worse, rejected |

### The JIT is a net loss here, and the earlier 24x claim was wrong

An earlier note in this session put the JIT at ~2,170 MIPS against ~90 MIPS for
the interpreter. That reading was taken from a run where the guest was **hung,
spinning in a tight loop** — the best possible case for a block JIT and wholly
unrepresentative. Measured during real gameplay the JIT reaches 222–236 MIPS
against the interpreter's 171, i.e. ~1.4x raw throughput, and it still loses on
frame rate.

The reason is visible in its own counters. Cold, it compiled **48,000 blocks
into 16,416 separate WebAssembly modules, 291 MB of generated code**, and was
still climbing after 320 s. A cache was therefore recorded for this exact
root/app/override combination (`tools/gen-jit-cache.mjs`, now parameterised by
`ROOT_ZIP`/`APP_ZIP`/`DLL_OVERRIDES`/`OUT_NAME`), giving 37,246 blocks and a
99.9% hit rate (`hits=47900 freshCompiled=50`). Frame rate still came out below
the interpreter, because `cachedInstalls` tracks `hits` one-for-one — every hit
re-installs its module rather than staying resident. Until that is fixed in
BoxedWine, the JIT should not be used for this workload.

### Where the remaining time goes

MIPS is pinned near 171 in *every* working interpreter configuration, so the
interpreter's throughput is the ceiling and frame rate is set purely by
instructions per frame. From the two resolution points, frame cost is roughly
**34 ms fixed + 63 ms pixel-dependent at 640x480**; at 320x240 the fixed part
already dominates, so further resolution cuts have little left to give. The
fixed part is not the desktop or emulator screen size (tested, no change) and
not audio (tested, no change).

At 320x240 the game spends ~8.6M emulated instructions per frame for 76,800
pixels — about 112 instructions per pixel, far above what a classic-mode
software renderer should need. The likely cause is the number of full-frame
passes between the game and the canvas (game buffer → SDL surface → GDI DIB
→ 8-bpp-to-32-bpp conversion → XImage → texture upload). Collapsing that chain
is the next real lever and much of it is Wine-side.

### 2026-08-17 (later): the OpenGL blocker was a canvas-ownership bug

The long-standing "10x10 WebGL backing buffer" symptom is **not** a consequence
of the NULL fixed-function calls. It is a window/canvas ownership bug in
BoxedWine, and it is now fixed.

Instrumenting every GL window creation gave the sequence outright:

```text
BOXEDWINE GL WINDOW: create 100 x 100   <- BoxedWine's probe context
BOXEDWINE GL WINDOW: create 1 x 1       <- Wine GLX probe drawable
BOXEDWINE GL WINDOW: create 10 x 10     <- Wine GLX probe drawable
BOXEDWINE GL WINDOW: create 10 x 10
BOXEDWINE GL WINDOW: create 640 x 400   <- the real game window
```

`glResizeWindow` was never called. The browser has one canvas and every
`SDL_CreateWindow` rebinds it, so BoxedWine's one-SDL-window-per-X-drawable
design let Wine's probe drawables resize the canvas out from under the real
window. Measured canvas timeline was `640x480 -> 0x0 -> 10x10 -> 0x0`.

Two changes in `source/opengl/sdl/sdlgl.cpp` fix it:

- `glCreateWindow` no longer materialises an SDL window on emscripten.
- `glMakeCurrent` reuses the single browser-owned `webWindow` and resizes it to
  the drawable being made current, and only when the size actually changed
  (it runs every frame; resizing the canvas recreates the WebGL context).

The canvas now settles at **640x400 and stays**, and the game window is drawn
at the correct size inside the Wine desktop.

### What is proven about OpenGL now

- Every `wglGetProcAddress` the game issues **succeeds** (FBOs, VAOs, samplers,
  `glGetStringi`). The `glad_glVertexPointer` NULL documented in the sections
  below no longer reproduces.
- GL draw calls run continuously — the `draw` counter reaches ~10,000 over a
  180 s run, roughly 66 draws/second.
- Of the fixed-function core, only `glFogf`, `glFogi` and `glTexEnvf` are
  unresolved under `LEGACY_GL_EMULATION`; emscripten implements all three in
  `libglemu.js` but does not publish them through `SDL_GL_GetProcAddress`, so
  they are now bound directly (guarded by `BOXEDWINE_LEGACY_GL_EMULATION`).

### Remaining OpenGL blocker

The run still ends in `RuntimeError: null function`, thrown directly from the
emscripten main loop (`MainLoop_runner` -> `runIter` -> `callUserCallback`) with
no wasm frames beneath it. Three candidate sources have been **ruled out** by
instrumentation:

- not a NULL `pgl*` host pointer (`BOXEDWINE GL CALLED-NULL` never fires),
- not a hole in the int99 dispatch table (`GL UNIMPLEMENTED dispatch slot`
  never fires; unset slots are now filled with a naming stub),
- not repeated GL re-resolution (`initSdlOpenGL` is now called once).

It is GL-specific: the same runtime runs the software path for 320 s without it.
The most likely remaining explanation is wasm-table churn — the JIT's
`addFunction`/`removeFunction` reclaiming a slot that is still referenced — which
would explain a null call target with no wasm frames. Diagnosis is hampered
because the `Jit` runtime does not forward guest stdout, while `Control` (which
does) is interpreter-only.

### Diagnostics added to BoxedWine

All behind `-DBOXEDWINE_LOG_MISSING_GL` (set only by the `jitLegacyGL` target):
unresolved entry points at load, called-NULL entry points, GL window
create/resize sizes, and unimplemented dispatch slots. The dispatch-slot filling
is unconditional and is a genuine robustness fix, not just a diagnostic.

**The shipped configuration is unaffected by any of this.** It runs on the
`Control` runtime (`build-gl/boxedwine.wasm`, unchanged since 2026-08-16 23:20),
and none of the BoxedWine rebuilds above touch it.

### Rejected runtime rebuilds

`multiThreadedJit` and `multiThreaded` were both rebuilt from the sibling
BoxedWine tree. Both link, but neither launches the guest in the browser: they
mount the zips and stop before `Launching /bin/wine`. Removing
`-sPROXY_TO_PTHREAD=1` from `multiThreaded` was not sufficient. `Control`
remains the only runtime that runs Wine reliably, and the packaged
`build-gl/boxedwine.*` files come from it.

## Objective and result

The objective is to run the fork's 32-bit Windows VibeBuild32 executable in its
640x480, 32-bpp OpenGL mode through Wine 11 and BoxedWine/WebGL in a browser.
Completion requires a real rendered frame and working input.

Current result: **failing, black/no real frame**. The old make-current access
violation is cleared. The newer `gl2ext` derivative root also clears the NULL
`glGetStringi` call: Wine's GL 2 extension-count query reaches BoxedWine and
returns zero. The process then reaches `glShadeModel`; the browser throws
`RuntimeError: null function`. WASM disassembly and resolver tracing prove the
NULL target is BoxedWine `pglShadeModel`; the canvas remains internally 0x0.

## Workspace identity

| Component | Identity | Important state |
| --- | --- | --- |
| `/Users/alonamir/dev/wine` | `acb4feb4eadab20724481d48071e7cd27d272fde` | Branch `vibe`; dirty and untracked work must be preserved. |
| `/Users/alonamir/dev/boxedwine` | `e6f66edc26fd33d81542ae206ca8968cbc02bb6f` | Branch `master`; dirty WebGL/OpenGL work; do not reset or clean. |
| `/Users/alonamir/dev/eduke32` | `673f07bd56631ca71a6afa572845f100bae8cd7c` | Branch `vibe`; heavily dirty; supplies `vibebuild32.exe` and licensed data. The ImGui GL2 guard discussed below is not applied. |
| `/Users/alonamir/dev/emsdk` | Checkout `15f08bfcc79fc1fe713dab39d0eb30ca71efc5d1`; emcc 4.0.23 (`7a5d93b50f6a3a35e85a0d2fc9e667b8498e6aed`) | Branch `main`; built the current Control runtime. |

The local Wine checkout is newer than the packaged Wine filesystem: local
sources use `WINE_OPENGL_DRIVER_VERSION` 38, while the Wine 11.0 root uses 37.
Do not copy local Wine 11.14-era modules into the packaged Wine 11.0 root.

At this checkpoint, `AGENTS.md`, `web-showcase/docs/`,
`web-showcase/serve_gl.mjs`,
`web-showcase/tools/`, the `build-gl` artifacts, and the essential sibling
BoxedWine fixes are not all committed. They survive a new session in this
workspace but not a clean clone. Preserve them and include the appropriate
source/docs/scripts—not the licensed or multi-gigabyte build outputs—in the
next requested commit.

## Exact artifact manifest

All paths are relative to `/Users/alonamir/dev/wine`. The artifacts are local
and currently untracked.

| Artifact | SHA-256 | Meaning |
| --- | --- | --- |
| `web-showcase/build-gl/boxedwine.js` | `2acb3471e2b4c20064f4b2ffd90eac8c6719a0c3fd31da8f56383f791d91db1a` | Patched Control glue; byte-identical to sibling `Build/Control`. |
| `web-showcase/build-gl/boxedwine.wasm` | `4a836d2267d708685ce6369bee5122bd6c36bc2e11c2483d921d201cd4222452` | Pthread, interpreter-only Control runtime; not the JIT runtime. |
| `web-showcase/build-gl/tinycore-wine11-parent-inline-webgl-pci-glxshim.zip` | `47e1d4d0961c62693960a756e8179ed2497a7784583c904de240c4d24e0c15fe` | Canonical regression root; still reproduces the ImGui NULL-call failure. |
| `web-showcase/build-gl/tinycore-wine11-parent-inline-webgl-pci-glxshim-gl2ext.zip` | `1bdc1c5b03f755ecd1aedef491f5fecfc2e2a18fe0f1d1e14f056e12810683cc` | Latest Wine 11.0 derivative; clears the NULL call and exposes the `glShadeModel` boundary. |
| `web-showcase/build-gl/netduke32.zip` | `b27df9cc54377a0b5c1579512768cad149b0ef65a69b6752cde1a053999dda69` | Application/data overlay used by both comparison runs. |
| `web-showcase/tools/patch-wine11-gl2-extension-count.mjs` | `d1ce16731c0860f0ff1484a8840ba7bc5d3f6b09fb9d1d8db6d837e7423b508e` | Hash/context-pinned script used to make the `gl2ext` inner-module change. |

Inner identities:

- Packaged `netduke32/netduke32.exe` is VibeBuild32 v1.3 and has SHA-256
  `149dc75560db8dd358bb50911fb5d480ef113335164d7e2c7045a0cc00cc5134`.
- Root `/opt/wine/lib/wine/i386-unix/winex11.so` has SHA-256
  `7fa3fe52a8d9c515b32bda094de96bb859ff7ecf89970f00dba946dcb184886d`.
- Root `/lib/libGL.so.1.2.0` has SHA-256
  `f09030c9fc8cbfa71e04985629217159aa61332363d014e12d39d993ea850c2f`.
- Canonical root `/opt/wine/lib/wine/i386-unix/opengl32.so` has SHA-256
  `60bb64157afc62067ee2298fac0a96943deaf63783efd30c91b8ad8c305eaea0`.
  The `gl2ext` root's corresponding module has SHA-256
  `a3dceaa1f93019a76da43c9b5019c9847b25abc385af2e20c6da47821649287b`.
  Only file offsets `0x6aac0` and `0x6aac1` differ (`74 65` to `90 90`).
- `tinycore-wine11-parent-inline-webgl-pci-glxfix.zip` is byte-identical to
  the canonical `glxshim` root. Use `glxshim` in new notes.
- `web-showcase/build-gl/tinycore-wine11.zip` is a symlink to the unmodified
  downloaded root (SHA-256
  `9393e49ea77e28d0223e43bbcb02f21976883cef14fcaa0faad8b2601cba005f`),
  not the current test root.

## Exact launch

Serve without caching:

```sh
cd /Users/alonamir/dev/wine
node web-showcase/serve_gl.mjs web-showcase/build-gl
```

Do not open bare `http://localhost:8082/`; its redirect selects the base root.
Use this explicit latest-diagnostic URL:

```text
http://localhost:8082/boxedwine.html?root=tinycore-wine11-parent-inline-webgl-pci-glxshim-gl2ext.zip&app=netduke32.zip&resolution=640x480&storage=memory&env=%22WINEDLLOVERRIDES:mscoree,mshtml=|WINEDEBUG:+seh,+wgl,+opengl%22&w=/home/username/.wine/dosdevices/c:/files/netduke32&p=netduke32.exe&args=-cfg%20netduke32.cfg%20-nosetup%20-g%20DUKE3D.GRP%20-v1%20-l1%20-s3&gl2ext_test=1
```

The corresponding guest command is `/bin/wine netduke32.exe -cfg
netduke32.cfg -nosetup -g DUKE3D.GRP -v1 -l1 -s3`, with the working directory
set to the packaged `netduke32` directory.

At the checkpoint, the relevant server was PID 87337 on port 8082 and Chrome
was PID 2220 with DevTools Protocol on port 9555. The latest page target was
`51B01077674CE3222AC4880198609D51`. These identities are ephemeral; check with
`lsof` and `/json/list` before relying on them.

## Last decisive result

The probe contexts (`0x1000`, `0x2000`) and game contexts (`0x3000`, `0x4000`)
now make and clear current successfully enough to reach the game's GL loader.
The old line below is absent after the BoxedWine unbind fix:

```text
warn:opengl:wglMakeCurrent wglMakeCurrent returned 0xc0000005
```

The canonical regression root demonstrates why the next fix was needed. With
`WINEDEBUG=+seh,+wgl,+opengl`, Wine rejects `glGetStringi`, synthesizes a
positive `GL_NUM_EXTENSIONS` value, and ImGui calls the NULL slot:

```text
warn:opengl:wrap_wglGetProcAddress Extension GL_VERSION_3_0 required for glGetStringi not supported
trace:opengl:glGetIntegerv pname 33307, data ...
trace:opengl:glGetIntegerv pname 33308, data ...
trace:opengl:glGetString name 7938
trace:opengl:glGetIntegerv pname 3379, data ...
trace:opengl:glGetIntegerv pname 32873, data ...
trace:opengl:glGetIntegerv pname 33309, data ...
trace:seh:dispatch_exception code=c0000005 (EXCEPTION_ACCESS_VIOLATION) flags=0 addr=00000000
trace:seh:dispatch_exception  info[0]=00000008
trace:seh:dispatch_exception eip=00000000 ... edx=00000007
Caught signal: SIGSEGV
```

`33309` is `GL_NUM_EXTENSIONS`. The crash stack and packaged PE prove the next
call:

```text
ESP                 0x7f30f868
[ESP] return        0x005a0261
[ESP+4] argument    0x00001f03 (GL_EXTENSIONS)
0x005a025b          calll *0x0165ee88
0x0165ee88          NULL glGetStringi function slot
```

The same disassembly calls the `glGetIntegerv` slot at `0x005a0231`, passing
`0x821d` (`GL_NUM_EXTENSIONS`), tests the positive result, then enters the loop
and makes the indirect call at `0x005a025b`. This corresponds exactly to
`/Users/alonamir/dev/eduke32/source/imgui/src/imgui_impl_opengl3.cpp` around
lines 415-423. The NULL `glGetStringi` call is proven, not only inferred from
log ordering.

The latest `gl2ext` root changes only the matched Wine 11.0 `opengl32.so`.
Its decisive sequence is:

```text
trace:opengl:glGetIntegerv pname 33309, data 7F30FA10
GLX: GL2 GL_NUM_EXTENSIONS -> 0
trace:opengl:glShadeModel mode 7425
Uncaught RuntimeError: null function
    at boxedwine.wasm:wasm-function[6081]:0x2a57ad
    at boxedwine.wasm:wasm-function[2189]:0xf6a62
    at boxedwine.wasm:wasm-function[5037]:0x25953a
    at boxedwine.wasm:wasm-function[5431]:0x270285
```

Chrome DevTools emitted both the console event and
`Runtime.exceptionThrown`. The guest output remained at exactly 111255 bytes
for several minutes. This run has no `wglMakeCurrent returned`, no guest
`c0000005`, and no `Caught signal`. Clearing the guest exception is real
progress, but the host-side NULL call is still a failure.

The latest canvas inspection returned internal `width=0`, `height=0`, CSS
`338x253`, and no real frame. The WebGL 1 context remained present, but there
was no frame or input to validate. Treat dimensions as run evidence, not a
fixed invariant.

## Local BoxedWine changes that matter now

The verified fix is in
`/Users/alonamir/dev/boxedwine/source/opengl/sdl/sdlgl.cpp`:

```cpp
if (contextId && (!context || !context->context)) {
    return false;
}
```

`contextId == 0` is a valid GLX unbind. The earlier unconditional NULL-context
guard rejected unbind, leaving Wine's deleted probe context current and causing
the old use-after-free in Wine 11.0 `context_sync_drawables()`.

The current code in `source/opengl/glcommon.cpp` special-cases
`GL_NUM_EXTENSIONS` to zero under Emscripten and logs:

```text
GLX: GL2 GL_NUM_EXTENSIONS -> 0
```

The canonical root never prints this marker because Wine 11.0
`dlls/opengl32/unix_wgl.c:get_integer()` returns its filtered
`ctx->extension_count` before calling the Unix driver. In the `gl2ext` root,
the branch at file offset `0x6aac0` is disabled, the query falls through the
guest driver, this marker is observed, and zero reaches ImGui. That dispatch
question is now resolved.

The exact diagnostic patch is
`web-showcase/tools/patch-wine11-gl2-extension-count.mjs`. It is pinned to the
canonical input/output hashes and surrounding machine code. It is a narrow
Wine 11.0 binary diagnostic, not permission to mix the newer local Wine ABI
into the root. Reproduction and content-equivalent repack commands are in
[reproduction.md](reproduction.md).

The next guest call is `glShadeModel(GL_SMOOTH)`. The full boundary is proven:

- EDuke32 `source/build/src/winlayer.cpp` calls `glShadeModel(GL_SMOOTH)`;
  `7425` is `0x1d01`, and no active `GL_FLAT` use was found.
- Guest `tools/opengl/gl.c` sends BoxedWine opcode 216 (`ShadeModel`).
- `source/opengl/glfunctions.h` and the generator macros in
  `source/opengl/glcommon.cpp` make `glcommon_glShadeModel()` call
  `pglShadeModel(ARG1)`.
- `source/opengl/sdl/sdlgl.cpp` initializes that pointer with
  `SDL_GL_GetProcAddress("glShadeModel")`.
- Emscripten's WebGL 1 proc table has no `glShadeModel` and returns NULL. The
  current build uses `FULL_ES2`, not legacy GL emulation.
- In WASM SHA-256 `4a836d22...`, disassembly at `0x2a5784` loads the function
  pointer and the reported fault PC `0x2a57ad` executes
  `return_call_indirect 0`.

The narrow compatible fix belongs in BoxedWine
`source/opengl/sdl/sdlgl.cpp`: add an Emscripten-only
`wasmGlShadeModel(GLenum)` fallback and assign it only when
`pglShadeModel == nullptr`. It should no-op for `GL_SMOOTH` and warn while
retaining smooth interpolation for other modes. WebGL/GLES2 interpolates
varyings smoothly by default; faithfully implementing `GL_FLAT` would require
shader-level work. Do not enable `LEGACY_GL_EMULATION` (it conflicts with
`FULL_ES2` and still implements this call as a no-op), and do not enable the
incomplete broad BoxedWine ES layer.

The packaged guest `lib/libGL.so.1` and `libGL.so.1.2.0` are identical to each
other (SHA-256
`f09030c9fc8cbfa71e04985629217159aa61332363d014e12d39d993ea850c2f`)
and differ from the sibling checkout's current untracked
`tools/opengl/libGL.so.1` (SHA-256
`92cfe73f20d6c2772c36b6f2376c83490e27f4337f8755235346b130f51c52fb`).
The packaged `glGetIntegerv` disassembly does issue int `0x99` opcode `0x20`
as expected, so the differing binary is a dispatch/provenance clue, not by
itself proof of the failure. Do not overwrite the packaged shim with the
sibling binary: its GLX resolver behavior is known to be older/wrong for this
run.

The sibling BoxedWine diff contains additional WebGL context ownership,
desktop-GL compatibility, and diagnostic changes. Inspect the full focused diff
before editing. Those changes are not yet represented by a complete patch in
this Wine repository.

## Latest Wine-only test (2026-08-17)

The exact supplied PE (`547dea93…33878`) was launched through the Wine-repo
`netduke32-wine-launcher.exe`; no EDuke32/VibeBuild32 source was edited or
built. The app package was `netduke32-v1.2.1-slotshim2.zip` (outer SHA
`b0805123f52b3e78db52f216896af2d8701fb177f746021c692cc28eed90276a`). The
root was the Wine-only diagnostic
`tinycore-wine11-parent-inline-webgl-pci-glxshim-legacyctxattrib-fixeddefaults-clientstate.zip`
(SHA `c047cd478c54a936a0a73e7abf2920d05ec45d4ff1f424b970428e96a40ea0f7`).

The client-state wrappers in the guest GL shim were temporarily replaced with
no-ops. This crossed the previous null-function boundary: the page reported a
640x480 canvas and a captured screenshot contained a non-black startup frame.
After several seconds the canvas returned to 10x10 and Wine logged drawable
release/context teardown. Therefore this is diagnostic progress, not a pass:
continuous rendering, two distinct frames, process liveness, and input remain
unproven. Do not broaden no-op patches without identifying the exact next
legacy GL call; fixed-function client state, matrices, fog values, and GLSL
1.20 shaders require a coherent compatibility layer.

A follow-up derivative (`...fixeddefaults-clientstate2.zip`, SHA
`162a0da44595079cecf3890026cad123a0e904341c747cfd1b34143187328844`) also
no-oped the vertex and texture pointer wrappers. It exited earlier and did not
produce a sustained canvas, so it is explicitly rejected as a fix.

## Immediate next action

Continue with Wine-repository artifacts only. Capture the next exact guest
return address around the post-client-state teardown, then decide whether a
semantically valid Wine/BoxedWine compatibility change is possible. Do not
edit, build, clean, reset, or otherwise experiment with `/Users/alonamir/dev/eduke32`
or any other sibling project unless the user explicitly authorizes that named
action. A real completion still requires a sustained non-black 32-bpp frame,
pixel updates over time, and keyboard/mouse input.

## Latest continuation: slotshim4 pointer boundary (2026-08-17)

The only executable in scope is `/Users/alonamir/games/netduke32_v1.2.1/netduke32.exe`,
SHA-256 `547dea93d40114dee7757a049f20e0f7659cbd0c221ae9cf4258338e94c33878`.
The Wine-only diagnostic artifacts are:

| Artifact | SHA-256 |
| --- | --- |
| `web-showcase/build-gl/netduke32-v1.2.1-slotshim4.zip` | `594fc6e205edc7d74286ede2b365ff1080d69d911c698cfb016bba6771146c42` |
| `web-showcase/build-gl/tinycore-wine11-parent-inline-webgl-pci-glxshim-legacyctxattrib-fixeddefaults.zip` | `d3d4cc92121be806ad4228c086818474f9f5ff3ccc772a00c1d4c074a8d027c7` |

The launcher clears the earlier sampler/sync NULL calls and translates legacy
pointer calls, but the run still faults after the second `glBufferData`:

```text
eip=00000000, info[0]=00000008
0x00539b4c: call *0x019ea2f4
return:     0x00539b52
slot:       0x019ea2f4 (glad_glVertexPointer)
```

The guest translation and direct call redirect did not remove the NULL target;
the remaining likely boundary is BoxedWine's host `pglVertexAttribPointer`
slot, or a runtime that lacks that handler. The blurry animated screenshot is
only a 10x10 (sometimes 0x0) WebGL backing buffer magnified by CSS, not a game
frame. Context teardown later returns it to 10x10/black. Sustained rendering,
temporal pixel changes, process liveness, and input remain unproven.

### Scope rule for future sessions

Only files under `/Users/alonamir/dev/wine` may be edited. Never edit, build,
clean, reset, stash, or otherwise experiment with `/Users/alonamir/dev/eduke32`
or `/Users/alonamir/dev/boxedwine` unless the user explicitly authorizes that
named action. Keep branch `vibe`; never rebuild or substitute the supplied PE.

The next safe action is Wine-repository-only instrumentation or binary
diagnostics to prove the host pointer slot. A matched sibling BoxedWine rebuild
requires explicit authorization and must not be performed implicitly.
