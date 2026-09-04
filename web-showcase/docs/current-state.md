# Current browser checkpoint

## 2026-09-04 13:27 IDT: cached NPOT renderer return promoted

Change: commit `c4149271` adds a skeleton-verified native fast path for both
side-effect-free return cases in `polymost_npotEmulation` at `0x00555790`.
The update/SSE/GL path remains interpreted, and `WASM_NO_NPOT_EARLY=1` is the
runtime rollback control. No sibling checkout was modified.

Observation: same-artifact A/B runs both reached E1L1, rendered real non-black
640x400 canvases, accepted W input, and had no `FATAL`, `RuntimeError`, or
`JITBAD`. The enabled arm measured late samples of about 111--131 FPS; the
disabled arm measured about 104--120 FPS. The published canonical URL
`http://localhost:8799/?WASM_TPUT=1&WW_ARGS=%2Fv1,%2Fl1&build=npot-cached-published-c4149271`
then reached `E1L1: HOLLYWOOD HOLOCAUST`, reported `input: ready`, and stayed
live through late samples of 107.0, 116.6, and 116.8 FPS. Its screenshot was
non-black gameplay at 640x400, hash `58f501b8`.

Published hashes are JS
`ec9fc0864c8de9f8242b84a84d2f927c7d3b4778ebfd001ae754afc68ee1a1f3`, WASM
`f02c1d991c597fd6e42651c74364baf016dfc62805fdb2f8c41b9ab19aa41a9f`, data
`b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`, index
`455e20ff86b48a6c3e880dd5558bc54c2f749845b2fee6ee7fa343407bd9bcc6`, and
worker `72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`.
Tracked source is clean at `c4149271`, apart from preserved untracked
build/cache artifacts. Port 8799 returned 200 with COOP/COEP/CORP headers.

## 2026-09-04 13:23 IDT: post-debug frame profile identifies next target

Observation: the published debug-message build was profiled at
`http://localhost:8799/?WASM_TPUT=1&WASM_IPAGE=1&WASM_IPAGE_FRAME=1&WASM_IPAGE_DETAIL=1&WW_ARGS=%2Fv1,%2Fl1&build=post-debug-profile-20260904`.
It reached its first frame at 11.9s, reached E1L1, rendered a real non-black
640x400 canvas, and had no `FATAL`, `RuntimeError`, or `JITBAD`. Late samples
reached 124.9--127.3 FPS. The frame-scoped exact miss leaders are now
`0x00555790`/`0x0055579c` (polymost NPOT state), followed by
`0x0055b6f0`, `0x00555610`, and `0x00529500` (renderer/SSE and sampler
families). The previously shipped `0x00609640` debug-message entry no longer
appears among the leaders, confirming that bypass is active.

No source or canonical artifact changed during this diagnostic. The next
action is a complete function-family experiment around the NPOT/sampler early
returns, with the same real-canvas and input gates; isolated entry seeding is
not being promoted based on this profile alone. Source remains clean at
`3095eb0c` apart from preserved untracked build/cache artifacts, and no sibling
checkout was modified.

## 2026-09-04 13:19 IDT: NPOT early-return probe rejected

Observation: a same-artifact browser A/B tested a skeleton-checked early return
for `polymost_npotEmulation` when its cached mode differs from the current mode.
Both arms reached `E1L1: HOLLYWOOD HOLOCAUST`, rendered real non-black 640x400
frames, accepted W input, and had no `FATAL`, `RuntimeError`, or `JITBAD`.
Late samples overlapped substantially: the probe measured about 111--128 FPS,
while `WASM_NO_NPOT_EARLY=1` measured about 98--121 FPS. This is not
reproducible evidence of a gain, so the probe was removed and not promoted.

The published bundle was rebuilt from the prior verified source and restored at
the canonical URL
`http://localhost:8799/?WASM_TPUT=1&WW_ARGS=%2Fv1,%2Fl1&build=debug-hook-published-3095eb0c`.
The restored WASM hash is
`1168a133a5b1429b605ac09f23a9aa3558cb952a7882f2198ccd08c844806c59`; JS,
data, index, and worker remain respectively
`ec9fc0864c8de9f8242b84a84d2f927c7d3b4778ebfd001ae754afc68ee1a1f3`,
`b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`,
`455e20ff86b48a6c3e880dd5558bc54c2f749845b2fee6ee7fa343407bd9bcc6`, and
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`.
Tracked source is clean at `3095eb0c`, apart from preserved untracked
build/cache artifacts; no sibling checkout was modified.

## 2026-09-04 13:08 IDT: disabled debug-message fast exit promoted

Change: commit `3095eb0c` adds a skeleton-verified native fast exit for
`buildgl_outputDebugMessage` at `0x00609640`. It bypasses only the exact
observed `debug-flags & 4 == 0` return path; enabling the flag falls back to
the guest formatter and allocator. `WASM_NO_DEBUG_MESSAGE=1` disables the
hook for A/B comparison. No sibling checkout was modified.

Observation: same-artifact A/B runs both reached E1L1 with real non-black
640x400 canvases and no `FATAL`, `RuntimeError`, or `JITBAD`. With the hook on,
late samples were 104--122 FPS; with `WASM_NO_DEBUG_MESSAGE=1`, they were
81--109 FPS. The published canonical run at
`http://localhost:8799/?WASM_TPUT=1&WW_ARGS=%2Fv1,%2Fl1&build=debug-hook-published-3095eb0c`
logged `native buildgl debug-message disabled path @ 00609640`, reached
`E1L1: HOLLYWOOD HOLOCAUST`, reported `input: ready`, accepted W down/up, and
produced screenshot hash `6c1978bc`. Late samples reached 110.2, 110.7, and
123.5 FPS; the run ended live with no fatal/runtime/JIT error.

Published hashes are JS
`ec9fc0864c8de9f8242b84a84d2f927c7d3b4778ebfd001ae754afc68ee1a1f3`, WASM
`1168a133a5b1429b605ac09f23a9aa3558cb952a7882f2198ccd08c844806c59`, data
`b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`, index
`455e20ff86b48a6c3e880dd5558bc54c2f749845b2fee6ee7fa343407bd9bcc6`, and
worker `72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`.
Tracked source is clean at `3095eb0c`, apart from preserved untracked
build/cache artifacts. Port 8799 returned 200 with COOP/COEP/CORP headers.

## 2026-09-04 12:54 IDT: verified fast renderer promoted as browser default

Change: commit `52b5acc6` makes the already-tested aggregate renderer path the
browser default. It enables the native qrhline, mhline, surface-blit, and
verified renderer hooks without requiring a query flag. The emergency rollback
is `WASM_NO_FAST_RENDER=1`; no sibling checkout was modified.

Observation: a fresh temporary-build run at
`http://localhost:8807/?WASM_TPUT=1&WW_ARGS=%2Fv1,%2Fl1&build=default-fast-promoted`
reached `E1L1: HOLLYWOOD HOLOCAUST` at 11.3s, reported `input: ready`, rendered
a real non-black 640x400 canvas, accepted W down/up, and sustained roughly
97--121 FPS through its 57s sample. A fresh canonical run at
`http://localhost:8799/?WASM_TPUT=1&WW_ARGS=%2Fv1,%2Fl1&build=fast-default-52b5acc6`
reached its first frame at 11.1s and logged late samples of 116.1, 124.1,
125.6, 122.0, 125.5, 131.0, and 120.0 FPS through 37s. It remained live with
`input: ready`, `audio: on`, and no `FATAL`, `RuntimeError`, or `JITBAD`.
The screenshot result was non-black gameplay at 640x400 (hash `58f501b8`).

Published artifact hashes are JS
`ec9fc0864c8de9f8242b84a84d2f927c7d3b4778ebfd001ae754afc68ee1a1f3`, WASM
`1627d37e680212f4c10910532dcc05dce75e13b471001e326413717a2cceb663`, data
`b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`, index
`455e20ff86b48a6c3e880dd5558bc54c2f749845b2fee6ee7fa343407bd9bcc6`, and
worker `72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`.
The tracked source is clean at `52b5acc6`, apart from the preserved untracked
build/cache artifacts listed by git status. Port 8799 returns 200 with COOP,
COEP, and CORP headers.

## 2026-09-03 17:56 IDT: larger JIT lookup cache rejected

Observation: a browser candidate increasing the direct-mapped JIT hint cache
from 1,024 to 4,096 slots reached `E1L1: HOLLYWOOD HOLOCAUST`, rendered the
real 640x400 32-bpp WebGL frame (`a227e01c`), and reported `input: ready`.
The candidate's late samples were 65--79 FPS in the paired runs, while the
baseline measured 53--77 FPS; the overlap and run-to-run variation provide
no reproducible gain. No `JITBAD`, `FATAL`, `RuntimeError`, or assertion was
observed. The cache-size change was discarded.

The canonical bundle was not changed. Canonical hashes remain JS
`ec9fc0864c8de9f8242b84a84d2f927c7d3b4778ebfd001ae754afc68ee1a1f3`, WASM
`fe9b426a0ee8001edc831d0189fc56d3dd306977bbb96c390603bf7d2e0ed625`, data
`b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`, index
`455e20ff86b48a6c3e880dd5558bc54c2f749845b2fee6ee7fa343407bd9bcc6`, and
worker `72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`.
Source is clean at `7da65114` apart from preserved untracked build/cache
artifacts; no sibling checkout was modified.

The stable test URL remains
`http://localhost:8799/?WASM_TPUT=1&WW_ARGS=%2Fv1,%2Fl1&build=baseline-restored`.

## 2026-09-03 17:50 IDT: renderer SSE entry expansion rejected

Observation: a candidate adding seven exact executable renderer/SSE entry
roots (`0x0052a862`, `0x00555796`, `0x0055b360`, `0x00555610`,
`0x005599c0`, `0x005f3af8`, and `0x00616711`) generated 170,760 blocks and
reached `E1L1: HOLLYWOOD HOLOCAUST` with a real 640x400 32-bpp WebGL canvas
(`a227e01c`). Its late samples were about 52--72 FPS, below the verified
baseline interval in matched runs, so it was rejected. The run showed
`jit_frac` about 94--96% and no `FATAL`, `RuntimeError`, or assertion; it was
not promoted without a completed differential gate and a reproducible gain.

The generated table was restored and the canonical bundle was not changed.
Canonical hashes remain JS
`ec9fc0864c8de9f8242b84a84d2f927c7d3b4778ebfd001ae754afc68ee1a1f3`, WASM
`fe9b426a0ee8001edc831d0189fc56d3dd306977bbb96c390603bf7d2e0ed625`, data
`b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`, index
`455e20ff86b48a6c3e880dd5558bc54c2f749845b2fee6ee7fa343407bd9bcc6`, and
worker `72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`.
Source is clean at `406daea3` apart from preserved untracked build/cache
artifacts; no sibling checkout was modified.

The stable test URL remains
`http://localhost:8799/?WASM_TPUT=1&WW_ARGS=%2Fv1,%2Fl1&build=baseline-restored`.

## 2026-09-03 17:40 IDT: browser IPC -O2 candidate rejected

Observation: rebuilding the verified AOT browser bundle with `CINT=-O2`
(instead of the shipped `-O1`) reached `E1L1: HOLLYWOOD HOLOCAUST`, rendered
the real 640x400 32-bpp WebGL frame (`a227e01c`), and reported
`input: ready`. Late samples in the 22-second run were roughly 45--60 FPS;
there was no reproducible improvement over the verified baseline, so the
candidate was rejected. No `JITBAD`, `FATAL`, `RuntimeError`, or assertion was
observed.

The candidate was not published. Canonical hashes remain JS
`ec9fc0864c8de9f8242b84a84d2f927c7d3b4778ebfd001ae754afc68ee1a1f3`, WASM
`fe9b426a0ee8001edc831d0189fc56d3dd306977bbb96c390603bf7d2e0ed625`, data
`b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`, index
`455e20ff86b48a6c3e880dd5558bc54c2f749845b2fee6ee7fa343407bd9bcc6`, and
worker `72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`.
Source is clean at `b44a2799` apart from preserved untracked build/cache
artifacts; no sibling checkout was modified.

The stable test URL remains
`http://localhost:8799/?WASM_TPUT=1&WW_ARGS=%2Fv1,%2Fl1&build=baseline-restored`.

## 2026-09-03 17:34 IDT: verified baseline restored and playable

Observation: after restoring the verified translator, a fresh browser run at
`http://localhost:8807/?WASM_TPUT=1&WW_ARGS=%2Fv1,%2Fl1&build=baseline-final-20260903`
reached `E1L1: HOLLYWOOD HOLOCAUST`, created a real 640x400 32-bpp WebGL
frame, and produced sustained samples around 48--70 FPS after startup. The
run loaded 170,757 translated blocks and showed no `JITBAD`, `FATAL`,
`RuntimeError`, or assertion. The final screenshot hash was `a227e01c`.

The candidate build was not published because its WASM hash differs from the
canonical bundle; the canonical server on port 8799 remains the known-good
published artifact. Canonical hashes are JS
`ec9fc0864c8de9f8242b84a84d2f927c7d3b4778ebfd001ae754afc68ee1a1f3`, WASM
`fe9b426a0ee8001edc831d0189fc56d3dd306977bbb96c390603bf7d2e0ed625`, data
`b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`, index
`455e20ff86b48a6c3e880dd5558bc54c2f749845b2fee6ee7fa343407bd9bcc6`, and
worker `72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`.
Source is clean at `acf6366b` apart from preserved untracked build/cache
artifacts; no sibling checkout was modified.

The test URL is
`http://localhost:8799/?WASM_TPUT=1&WW_ARGS=%2Fv1,%2Fl1&build=baseline-restored`.

## 2026-09-03 17:35 IDT: flag-liveness AOT candidate rejected by verifier

Observation: a conservative generator experiment reduced emitted `set_lazy`
operations from 181,819 to 112,729 with the same 170,757 translated blocks.
The browser candidate did not pass the required differential gate: fresh
`WASM_JIT_VERIFY=1` runs reported repeated `JITBAD` mismatches (including
`0x007bbe80` and `0x0052f2d4`), and a non-verifier run later aborted during
startup with guest memory exhaustion and an ImGui assertion. The candidate was
discarded; the generator and generated table were restored to the verified
implementation. It was never published.

The canonical bundle was not changed. It remains JS
`ec9fc0864c8de9f8242b84a84d2f927c7d3b4778ebfd001ae754afc68ee1a1f3`, WASM
`fe9b426a0ee8001edc831d0189fc56d3dd306977bbb96c390603bf7d2e0ed625`, data
`b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`, index
`455e20ff86b48a6c3e880dd5558bc54c2f749845b2fee6ee7fa343407bd9bcc6`, and
worker `72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`.
Source is clean at `fc7a2029` apart from preserved untracked generated/build
artifacts; no sibling checkout was modified.

The stable test URL remains
`http://localhost:8799/?WASM_TPUT=1&WW_ARGS=%2Fv1,%2Fl1&build=baseline-restored`.


## 2026-09-03 17:08 IDT: compiler and XMM fast-path candidates rejected

Observation: two production browser candidates were built and exercised
against the direct `/v1,/l1` path at port 8807. Whole-module `-O3 -flto`
reached E1L1 and preserved the real 640x400 gameplay canvas, but did not beat
the shipped baseline. A narrower XMM 4/8/16-byte load/store specialization
also reached E1L1 with the same rendered scene, but its steady samples were
materially below the baseline during the run. Neither candidate was promoted;
the source was restored to the verified helper implementation. No
`FATAL`, `RuntimeError`, or `JITBAD` was observed in the completed candidate
runs.

The canonical bundle was not changed. It remains JS
`ec9fc0864c8de9f8242b84a84d2f927c7d3b4778ebfd001ae754afc68ee1a1f3`, WASM
`fe9b426a0ee8001edc831d0189fc56d3dd306977bbb96c390603bf7d2e0ed625`, data
`b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`, index
`455e20ff86b48a6c3e880dd5558bc54c2f749845b2fee6ee7fa343407bd9bcc6`, and
worker `72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`.
Source is clean at `9e3379f3` apart from preserved untracked generated/build
artifacts; no sibling checkout was modified.

The stable test URL remains
`http://localhost:8799/?WASM_TPUT=1&WW_ARGS=%2Fv1,%2Fl1&build=baseline-restored`.


## 2026-09-03 16:51 IDT: NP2 mapper default rejected after clean reruns

Observation: the existing skeleton-checked non-power-of-two single-column
mapper hooks were tested on the temporary candidate at
`http://localhost:8807/?WASM_TPUT=1&WW_ARGS=%2Fv1,%2Fl1&build=np2-clean`.
They reached `E1L1: HOLLYWOOD HOLOCAUST`, loaded both
`vlineasm1nonpow2` and `mvlineasm1nonpow2`, and produced the same real
640x400 gameplay image (`a227e01c`) without `FATAL`, `RuntimeError`, or
`JITBAD`. The initial paired sample looked about 3% faster, but clean longer
runs varied down to roughly 50--70 FPS on the same artifact, so the result is
not reproducible evidence and the default change was rejected. The hook
remains available only through `WASM_EXPERIMENT_NP2=1`.

The canonical bundle was not changed. It remains JS
`ec9fc0864c8de9f8242b84a84d2f927c7d3b4778ebfd001ae754afc68ee1a1f3`, WASM
`fe9b426a0ee8001edc831d0189fc56d3dd306977bbb96c390603bf7d2e0ed625`, data
`b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`, index
`455e20ff86b48a6c3e880dd5558bc54c2f749845b2fee6ee7fa343407bd9bcc6`, and
worker `72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`.
Source commit is `66d0e7ac`; tracked source is clean after the rejected
experiment, while preserved untracked generated/build artifacts remain. No
sibling checkout was modified.

The stable test URL is
`http://localhost:8799/?WASM_TPUT=1&WW_ARGS=%2Fv1,%2Fl1&build=baseline-restored`.


## 2026-09-03 16:32 IDT: frame-scoped SSE entry batch rejected

Observation: frame-scoped `WASM_IPAGE_DETAIL=1` identified repeated executable
misses at `0x0055b6f0`, `0x00555610`, and `0x005599c0`, so a candidate seeded
those entries together with the earlier renderer entries. It compiled 170,767
blocks, reached `E1L1: HOLLYWOOD HOLOCAUST`, and produced the same real
640×400 gameplay screenshot (`a227e01c`), but its late samples fell to roughly
82–93 FPS. It was not promoted. The frame-scoped miss detail confirms a broad
SSE/render family rather than one safe missing entry; isolated entry seeding is
not the next lever.

The canonical bundle was not changed and remains the known-good default. Its
hashes are JS
`ec9fc0864c8de9f8242b84a84d2f927c7d3b4778ebfd001ae754afc68ee1a1f3`, WASM
`fe9b426a0ee8001edc831d0189fc56d3dd306977bbb96c390603bf7d2e0ed625`, data
`b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`, index
`455e20ff86b48a6c3e880dd5558bc54c2f749845b2fee6ee7fa343407bd9bcc6`, and
worker `72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`.
Source commit is `80abd5e1`; tracked source is clean, preserved untracked
generated/build artifacts remain, and no sibling checkout was modified. The
stable test URL remains
`http://localhost:8799/?WASM_TPUT=1&WW_ARGS=%2Fv1,%2Fl1&build=baseline-restored`.

## 2026-09-03 16:26 IDT: GDI AOT default rejected after paired browser gate

Observation: the opt-in 8-block `gdi32.dll!WidenPath` AOT slice and its
same-artifact `WASM_NO_GDI32_JIT=1` control both reached E1L1, rendered the
identical screenshot (`a227e01c`), and remained free of `FATAL`/`RuntimeError`.
The apparent 3–5 FPS advantage in the first pair did not survive the next
canonical pair: default-on ended around 93–100 FPS and opt-out around 94–100
FPS. The candidate is therefore neutral and is not enabled by default.

The runtime now honors `WASM_NO_GDI32_JIT=1` when the GDI experiment is
explicitly requested. The browser default remains the verified executable AOT
plus OpenGL path. Source is on `vibe` with the tracked change uncommitted at
this checkpoint; preserved untracked generated/build artifacts remain, and no
sibling checkout was modified. The currently published artifact hashes are JS
`ec9fc0864c8de9f8242b84a84d2f927c7d3b4778ebfd001ae754afc68ee1a1f3`, WASM
`fe9b426a0ee8001edc831d0189fc56d3dd306977bbb96c390603bf7d2e0ed625`, data
`b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`, index
`455e20ff86b48a6c3e880dd5558bc54c2f749845b2fee6ee7fa343407bd9bcc6`, and
worker `72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`.

Final canonical verification URL for this checkpoint:
`http://localhost:8799/?WASM_TPUT=1&WW_ARGS=%2Fv1,%2Fl1&build=baseline-restored`.
The 16:25 run logged `E1L1: HOLLYWOOD HOLOCAUST`, `jit_frac` near 94%, and
live 640×400 gameplay with the yellow crosshair, weapon, and HUD. This is an
observation; no FPS improvement is claimed for the rejected GDI experiment.

## 2026-09-03 16:14 IDT: name-resolved profile narrows the next FPS target

Observation: a profile-enabled normal AOT browser candidate was run at
`http://localhost:8807/?WASM_TPUT=1&WASM_PROF=1&WW_ARGS=%2Fv1,%2Fl1&build=profile-names`.
It reached `E1L1: HOLLYWOOD HOLOCAUST`, produced a non-black 640×400 gameplay
canvas, and stayed live without `FATAL` or `RuntimeError`. The resolved samples
showed diffuse `gdi32.dll!WidenPath`, ntdll heap/relocation/character helpers,
and executable renderer sites around `0x0055xxxx` and `0x0061xxxx`; there was
no single missing mapper dominating the frame. This rules out promoting the
already-rejected narrow GDI or isolated entry-seed candidates as an FPS fix.

The canonical published bundle was not changed. Its hashes remain JS
`ec9fc0864c8de9f8242b84a84d2f927c7d3b4778ebfd001ae754afc68ee1a1f3`, WASM
`ed0a9a819942dec48c424c6415cabc5f80f18b616957669ae5f199e97f29ae14`, data
`b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`, index
`455e20ff86b48a6c3e880dd5558bc54c2f749845b2fee6ee7fa343407bd9bcc6`, and
worker `72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`.
Source commit is `afd6c167`; tracked source is clean, preserved untracked
generated/build artifacts remain, and no sibling checkout was modified.

The next work hypothesis is a broader, verified optimization of the dynamic
renderer/SSE path or a measured Wine support routine, with differential and
real-canvas gates required before promotion. The stable test URL remains
`http://localhost:8799/?WASM_TPUT=1&WW_ARGS=%2Fv1,%2Fl1&build=final-safe-aot`.

## 2026-09-03 16:05 IDT: FPS probes rejected; canonical build preserved

Observation: three isolated candidates were tested against the published
browser path. `XOPT=-O3` rendered E1L1 but ended around 97 FPS, below the
shipped run's roughly 103 FPS. Explicit seeds for renderer misses
`0x00555790` and `0x00609640` compiled 170,759 blocks (one additional block)
and rendered correctly, but did not materially lower steady `kinsn/frame`
(`~760` versus `~763`) or improve FPS. A focused msvcrt AOT slice loaded 143
blocks and rendered E1L1 without a fault, but also did not improve the matched
late-frame interval. The full msvcrt AOT path remains rejected because its
earlier control run faulted before the first frame with an out-of-bounds
memory access.

The canonical bundle was not changed. It remains built with the verified
`GENBLK=1`, browser `XOPT=-O2` configuration at source commit `bceabc64`;
tracked source is clean, preserved untracked generated/build artifacts remain,
and no sibling checkout was modified. Published hashes are JS
`ec9fc0864c8de9f8242b84a84d2f927c7d3b4778ebfd001ae754afc68ee1a1f3`, WASM
`ed0a9a819942dec48c424c6415cabc5f80f18b616957669ae5f199e97f29ae14`, data
`b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`, index
`455e20ff86b48a6c3e880dd5558bc54c2f749845b2fee6ee7fa343407bd9bcc6`, and
worker `72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`.

The exact canonical verification URL remains
`http://localhost:8799/?WASM_TPUT=1&WW_ARGS=%2Fv1,%2Fl1&build=final-safe-aot`.
The profile run reached `E1L1: HOLLYWOOD HOLOCAUST`, a real non-black 640×400
canvas, and live flips; no `FATAL` or `RuntimeError` occurred. The new profile
evidence identifies the next work as the executable renderer/SSE miss regions,
not the rejected compiler, mapper-seed, or broad CRT switches. This last
sentence is a work hypothesis; the measured results above are observations.

## 2026-09-03 15:48 IDT: published stable JIT/WASM FPS build

Observation: the browser build was rebuilt with `GENBLK=1` and the generated
msvcrt AOT table compiled in, but msvcrt AOT execution remains disabled. An
explicit `WASM_MSVCRT_JIT=1` control run loaded 21,145 blocks and failed before
the first frame with `FATAL worker exception: memory access out of bounds`, so
that experimental path was not enabled for the published test build. The
source commit is `677be4c3`; tracked source is clean, while preserved
untracked build artifacts remain in the worktree. No sibling checkout was
modified.

Published artifacts in `/Users/alonamir/.webwine-work/web`: JS
`ec9fc0864c8de9f8242b84a84d2f927c7d3b4778ebfd001ae754afc68ee1a1f3`, WASM
`ed0a9a819942dec48c424c6415cabc5f80f18b616957669ae5f199e97f29ae14`, data
`b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`, index
`455e20ff86b48a6c3e880dd5558bc54c2f749845b2fee6ee7fa343407bd9bcc6`, and
worker `72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`.

Verification URL:
`http://localhost:8799/?WASM_TPUT=1&WW_ARGS=%2Fv1,%2Fl1&build=final-safe-aot`.
The clean end-to-end run logged `wasm_x86: JIT 170758 translated blocks
loaded`, `Application parameters: /v1 /l1`, and `E1L1: HOLLYWOOD HOLOCAUST`;
the screenshot showed a real non-black 640×400 rooftop gameplay frame with
weapon, HUD, and yellow crosshair. Later samples measured approximately
99–113 FPS (about 103 FPS), with `jit_frac=93.8%`, and no `FATAL`,
`RuntimeError`, or load hang. This is an observation; the unsafe msvcrt AOT
fault is not hypothesized to be fixed.

## 2026-09-03 15:19 IDT: restored tinted OpenGL crosshair and fast-test arguments

Root cause: the browser bundle linked stale `opengl32` unix-thunk objects from
before `f86f2b7b`, the existing BGRA-to-RGBA fix for tinted ART tiles. The
native GL shortcut also bypassed `glTexSubImage2D`, so the fix was unreachable.
The texture-upload shortcut was removed; hot draw/state shortcuts remain native.
The browser pre-js now installs `WW_ARGS` before Emscripten snapshots argv, so
`/v1,/l1` starts directly in E1L1. The source tree remains dirty from
preserved untracked build artifacts; no sibling checkout was modified.

Published artifacts in `/Users/alonamir/.webwine-work/web`: JS
`ec9fc0864c8de9f8242b84a84d2f927c7d3b4778ebfd001ae754afc68ee1a1f3`, WASM
`2a1a3d8622bd75bc32dcabc39d44051e1f849864e13ca11e8dbfdba1eceaa279`, data
`b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`, index
`455e20ff86b48a6c3e880dd5558bc54c2f749845b2fee6ee7fa343407bd9bcc6`, and
worker `72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`.

Verification URL:
`http://localhost:8799/?WASM_TPUT=1&WW_ARGS=%2Fv1,%2Fl1&build=bgra-published`.
The run logged `Application parameters: /v1 /l1` and `E1L1: HOLLYWOOD
HOLOCAUST`, reached a real non-black 640×400 gameplay canvas, and the
screenshot showed the actual yellow crosshair at screen center. Rendering was
live at roughly 55–67 FPS during the 23-second direct-game test, with no
`FATAL`, `RuntimeError`, or load hang.

## 2026-09-03 14:27 IDT: audio underrun telemetry verified during active play

The browser page now reports AudioWorklet underruns when they occur, rather
than showing only that the context opened. This is diagnostic UI only; it does
not alter the audio data path. The source tree remains dirty only from
preserved untracked build artifacts; no sibling checkout was modified.

Verification: after publishing the matching page to
`/Users/alonamir/.webwine-work/web`, a fresh run at
`http://localhost:8799/?WASM_TPUT=1&build=audio-check` reached a real 640×400
frame at `10.4s`, reported `audio: on`, accepted Enter and W down/up, and
reported no underrun count through 35 seconds of active rendering. Guest
rendering remained live at about 140–162 FPS in the tested interval, with no
`FATAL`, `RuntimeError`, or hang. The WASM and JS artifacts were unchanged from
the preceding verified OpenGL-presentation fix.

## 2026-09-03 14:24 IDT: fixed OpenGL presentation leaving the page on “Still starting Wine”

Root cause: the browser page enabled `WW_GL=1` but also forcibly set
`WASM_NO_GLPRESENT=1`. Wine therefore rendered and processed input, while
`webwine_gl_present()` returned before posting the `glframe` message that
updates the visible page canvas. The contradictory override was removed from
`webwine/browser/index.html`; explicit `?WASM_NO_GLPRESENT=1` remains available
for diagnostics. The source tree remains dirty only from preserved untracked
build artifacts; no sibling checkout was modified.

Published artifact: `/Users/alonamir/.webwine-work/web/index.html` now matches
the source at SHA-256
`5c8ad0fca95f24233443eb0545f759dafea6676b58c3b396885add707974cac7`.
WASM remains
`ecb8e7207b616af553c6ad833a6fd011b1c8114434c7e138ac5a72bc02f8b5ce` and JS
remains `eccd144dad0ffc11b1bc650fd2ee72b6a12fa329610a60d8f11beb33a6fc518e`.

Verification URL:
`http://localhost:8799/?WASM_TPUT=1&build=final-gl-present`. A fresh Chrome
run reached `first-frame: 10.6s`, produced a screenshot-backed non-black
640×400 canvas (`shotBytes=6613`, hash `9239273`), reported
`input: ready`, `audio: on`, and delivered `wasm_input` SDL Enter plus W key
down/up events. Guest `FPSSAMPLE` remained about 130–156 FPS during movement;
there was no `FATAL`, `RuntimeError`, or load-stage hang. The canonical server
returned 200 with COOP/COEP headers. This is the first checkpoint where the
visible page frame, keyboard path, and running Wine state are all verified
together.

## 2026-09-03 12:56 IDT: canonical server restarted after connection refusal

Observation: the reported `Still starting Wine` page was caused by the
canonical port 8799 process being down; a fresh check returned
`net::ERR_CONNECTION_REFUSED`. The COOP/COEP server was restarted from the
published artifact directory, and `http://localhost:8799/` now returns 200
with `Cross-Origin-Opener-Policy: same-origin`,
`Cross-Origin-Embedder-Policy: require-corp`, and `Cache-Control: no-store`.
The source tree remains dirty from preserved untracked build artifacts; no
sibling checkout was modified.

Verification: served WASM SHA-256 is
`ecb8e7207b616af553c6ad833a6fd011b1c8114434c7e138ac5a72bc02f8b5ce`, matching
the promoted fast build. A fresh 18-second browser run at
`http://localhost:8799/?build=server-live-check` loaded the worker, logged
`JIT 170758 translated blocks loaded`, initialized OpenGL 3.3 and audio, and
logged the logo slowdown warning without a worker/bootstrap/runtime error. It
did not produce a frame in that short no-key window because the game was still
waiting at its title sequence; the normal test requires pressing Enter after
the title sequence. The browser auto-Enter experiment was removed because
SAB events cannot wake that blocked wait reliably.

## 2026-09-03 01:42 IDT: fast default promoted — OpenGL plus JIT exceeds 70 FPS

Change: the browser page now defaults to the verified 32-bpp OpenGL renderer
(`WW_GL=1`) and AOT x86-to-WASM translator (`WASM_JIT=1`). URL environment
overrides remain available, including `WASM_NO_JIT=1` and `WW_GL=0`. The
single-column `vlineasm1`/`mvlineasm1` hooks remain enabled; unsafe
`mvlineasm4`, `surfspan`, and `qrhline` experiments remain opt-in. The source
tree remains dirty from preserved untracked build artifacts; no sibling
checkout was modified.

Verification: exact plain default URL
`http://localhost:8799/?WASM_TPUT=1&build=default-fast` served WASM SHA-256
`ecb8e7207b616af553c6ad833a6fd011b1c8114434c7e138ac5a72bc02f8b5ce`, JS
`eccd144dad0ffc11b1bc650fd2ee72b6a12fa329610a60d8f11beb33a6fc518e`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index `10c22497509b4392bd1d52c27c2450510015e700ed648dfcd0a8e685bcd17f32`.
The fresh 80-second run received both Enter events, logged
`wasm_x86: JIT 170758 translated blocks loaded`, rendered a real non-black
640x400 canvas, and reached `FPSSAMPLE t=79.5 ... fps=100.8` with nearby
samples from 75.6 to 100.8 FPS. No `FATAL`, `RuntimeError`, `JITBAD`, or
load-percentage hang occurred; the canvas remained live and input reported
`keys 4, mouse 2`.

Observations: the default path now reaches the game quickly and clears the
requested 70 FPS threshold in the tested browser session. Hypothesis: the
remaining variation is browser/host load and the emulator's dynamic CON work,
not the former software mapper bottleneck.

## 2026-09-03 00:24 IDT: single-column v1/l1 default enabled and load-stage hang cleared

Change: browser builds now arm the verified native `vlineasm1` and
`mvlineasm1` hooks independently of the broader experimental render switch.
This targets the remaining single-column mapper work without enabling the
unrelated risky render hooks. `WASM_NO_FAST_SINGLE_COLUMNS=1` disables both
hooks for regression comparison. The source tree remains dirty from
preserved untracked build artifacts; no sibling checkout was modified.

Verification: exact URL
`http://localhost:8799/?WASM_TPUT=1&build=v1l1-default` served WASM SHA-256
`ec9f319d8ddc49a93c73003f3ab79c68290f63280de54c1c651622d3ff56fd68`, JS
`7e9db027632fdb92516f47aee768ff79d087cb53bace60fb4947fef0a4ccecd7`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index `623fafa969f1dfbb819d5ceb7eac013ae802d52ff394c0c4e464ddbb8da479e4`.
The 115-second run reached a real non-black 320x200 canvas with
`first-frame: 47.0s`, accepted both Enter events, and remained live through
`FPSSAMPLE t=114.2` at approximately 24.9 FPS. Decisive lines included
`native vlineasm1 entry`, `native mvlineasm1`, `v1=2/400`, and
`mv1=98/1487`; no `UNIMPLEMENTED opcode`, `FATAL`, `RuntimeError`, or
load-percentage hang occurred. The canvas result was live gameplay output.

Observations: single-column hooks are reached during rendering and the
browser no longer stalls at the reported 79% stage. Hypothesis: the remaining
FPS ceiling is elsewhere in the interpreter/render path; this change does not
enable the unsafe four-column masked mapper or full SSE/x87 translation.

## 2026-09-03 06:20 IDT: interpreter-safe column default verified past 94%

Change: both native four-column mapper families are now opt-in in browser
builds (`WASM_FAST_COLUMNS=1` and `WASM_FAST_MVLINE=1`). The default keeps the
guest mapper implementations, eliminating the repeated load-stage deadlock;
the other verified native hooks remain active. The source tree remains dirty
from preserved untracked build artifacts; no sibling checkout was modified.

Verification: exact URL
`http://localhost:8799/?WASM_TPUT=1&build=interpreter-columns` served WASM
SHA-256 `eaf19f0d4cd40e72d2b66eae95b05d33e309beac45b1de26ea98c34613ae88c0`, JS
`cd92367247fdcfb9dc9e8644cdf32ca9e8bccfd5cebc80355c3ca13d8a6e5973`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index `623fafa969f1dfbb819d5ceb7eac013ae802d52ff394c0c4e464ddbb8da479e4`.
The fresh 105-second run delivered both Enter events and remained live through
94.2s at 25.9 FPS, with `vl=0/0`, `mv=0/0`, and no `UNIMPLEMENTED opcode`,
`FATAL`, `RuntimeError`, or hang. The canvas was a real non-black 320x200
frame.

## 2026-09-03 04:25 IDT: masked mapper removed from browser default

Change: the browser no longer arms the native `mvlineasm4` masked mapper by
default. It is available only with `WASM_FAST_MVLINE=1`; the verified unmasked
`vlineasm4` mapper remains native, with its pathological-span preflight. This
removes the last accelerated mapper implicated in the repeated 94% load stall
while retaining the majority of the column-rendering speedup. The source tree
remains dirty from preserved untracked build artifacts; no sibling checkout was
modified.

Verification: exact URL
`http://localhost:8799/?WASM_TPUT=1&build=vline-public` served WASM SHA-256
`1334f860a3cff05f8812244405f76f003952c48af2c60c52f7ec546c15a22b30`, JS
`cd92367247fdcfb9dc9e8644cdf32ca9e8bccfd5cebc80355c3ca13d8a6e5973`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index `623fafa969f1dfbb819d5ceb7eac013ae802d52ff394c0c4e464ddbb8da479e4`.
The fresh 105-second run delivered both Enter events, reached a real non-black
320x200 canvas, crossed `Cache size increased by 1024 to new max of 2048
entries` at 85.4062s, and continued at 33.8--58.8 FPS afterward. No
`UNIMPLEMENTED opcode`, `FATAL`, `RuntimeError`, or hang was observed.

## 2026-09-03 03:05 IDT: relocation guard for masked four-column mapper

Change: `mvlineasm4` and its dispatcher now reject callers in the relocatable
compiled-CON arena before entering the native self-modifying loop. Normal
masked spans remain native; dynamic-CON calls execute the original guest code.
The existing bounded `vlineasm4` fallback and default-off libdivide shortcut
remain in force. The source tree remains dirty from preserved untracked build
artifacts; no sibling checkout was modified.

Verification: exact URL
`http://localhost:8799/?WASM_TPUT=1&build=columns-mv-guarded` served WASM
SHA-256 `d19febb8721c72aa86b864a8ab3e2fad063b47d573f247fcf62b10054eecce36`,
JS `cd92367247fdcfb9dc9e8644cdf32ca9e8bccfd5cebc80355c3ca13d8a6e5973`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index `623fafa969f1dfbb819d5ceb7eac013ae802d52ff394c0c4e464ddbb8da479e4`.
The fresh 105-second run delivered both Enter events, reached a real non-black
320x200 canvas, crossed `Cache size increased by 1024 to new max of 2048
entries` at 81.9943s, and continued at 56.9--60.9 FPS afterward. No
`UNIMPLEMENTED opcode`, `FATAL`, `RuntimeError`, or hang was observed.

## 2026-09-03 01:55 IDT: bounded four-column mapper fallback

Change: `vlineasm4` now preflights its carry-terminated span length. If a
relocated or partially patched call would require more than 65,536 iterations,
the native hook declines before modifying guest memory and the original guest
loop runs instead. Normal spans remain native; libdivide remains disabled by
default. The source tree remains dirty from preserved untracked build
artifacts; no sibling checkout was modified.

Verification: exact URL
`http://localhost:8799/?WASM_TPUT=1&build=columns-bounded` served WASM SHA-256
`8494df7c4fa67f12dd7950e26d98b95c73f4d37ea5a965791557f6f03ae1a512`, JS
`cd92367247fdcfb9dc9e8644cdf32ca9e8bccfd5cebc80355c3ca13d8a6e5973`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index `623fafa969f1dfbb819d5ceb7eac013ae802d52ff394c0c4e464ddbb8da479e4`.
The fresh 90-second run delivered both Enter events, reached a real non-black
320x200 canvas, crossed the cache resize at 85.8812s, and continued at 54.9--61.9
FPS afterward. No `UNIMPLEMENTED opcode`, `FATAL`, `RuntimeError`, or hang was
observed.

## 2026-09-03 00:58 IDT: definitive stable browser default

Change: the libdivide native shortcut is disabled by default again. It remains
available only with `WASM_FAST_LIBDIV=1`; the default browser configuration
uses the verified four-column mapper hooks and the original libdivide guest
implementation. This removes the remaining native shortcut implicated in the
reported 79% load hang.

Verification: the exact URL
`http://localhost:8799/?WASM_TPUT=1&build=columns-final-stable` served WASM
SHA-256 `bb1a7dcd272a9a16681a91a8c9b21d56b7ea254334405e2876fd0a13ce218d84`,
JS `cd92367247fdcfb9dc9e8644cdf32ca9e8bccfd5cebc80355c3ca13d8a6e5973`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index `623fafa969f1dfbb819d5ceb7eac013ae802d52ff394c0c4e464ddbb8da479e4`.
The source tree remains dirty from preserved untracked build artifacts; no
sibling checkout was modified.

Observation: a fresh 90-second run delivered both Enter events, reached a real
non-black 320x200 canvas, crossed `Cache size increased by 1024 to new max of
2048 entries` at 85.9247s, and continued rendering at 54.9 FPS after the
transition. There was no `UNIMPLEMENTED opcode`, `FATAL`, `RuntimeError`, or
hang; the final `initial thread run returned (eip=000000e9)` occurred during
normal harness shutdown.

## 2026-09-03 00:18 IDT: guarded libdivide fast path crosses level load

Change: retain the fast browser libdivide cache for ordinary executable
callers, but route callers in the relocatable `0x03xxxxxx` compiled-CON arena
through the original guest implementation. This avoids nested native execution
while CON code is being relocated, which caused the observed 77--93% hangs.

Verification: the bundle served at
`http://localhost:8799/?WASM_TPUT=1&build=columns-libdiv-safe-long` with WASM
SHA-256 `725b248d1cb0d04925d51de9aa88bcd7af860e5a384cd72541a6ddb8a3e7b4a7`,
JS `cd92367247fdcfb9dc9e8644cdf32ca9e8bccfd5cebc80355c3ca13d8a6e5973`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index `623fafa969f1dfbb819d5ceb7eac013ae802d52ff394c0c4e464ddbb8da479e4`.
The source tree remains dirty from preserved untracked build artifacts; no
sibling checkout was modified.

Observation: a fresh run configured for 150 seconds (the guest ended normally
at about 86s) reached a real non-black 320x200 canvas with first frame at
39.7s, delivered the Enter events, crossed
`Cache size increased by 1024 to new max of 2048 entries` at 85.5478s, and
continued through the transition. No `UNIMPLEMENTED opcode`, `FATAL`,
`RuntimeError`, or hang occurred; the final `initial thread run returned
(eip=000000e9)` was normal harness/game shutdown.

## 2026-09-02 23:10 IDT: libdivide rollback at 77% cache boundary

Observation: the previously promoted URL
`http://localhost:8799/?WASM_TPUT=1&build=columns-libdiv` accelerated the early
load but hung at the 77% level-load boundary. The failure is isolated to the
new browser-default libdivide shortcut; the four-column mapper hooks were not
changed.

Change: browser libdivide is now opt-in via `WASM_FAST_LIBDIV=1` and remains
disablable with `WASM_NO_FAST_LIBDIV=1`. The default bundle is the stable
columns-only configuration. The source tree remains dirty because preserved
build artifacts are untracked; no sibling checkout was modified.

Verification: the rebuilt bundle served at
`http://localhost:8799/?WASM_TPUT=1&build=columns-stable` with WASM SHA-256
`c6f013faa3354907f3a846688b6f28c80e101e73cc96db4cf90420d541a6be58`, JS
`cd92367247fdcfb9dc9e8644cdf32ca9e8bccfd5cebc80355c3ca13d8a6e5973`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index `623fafa969f1dfbb819d5ceb7eac013ae802d52ff394c0c4e464ddbb8da479e4`.
The fresh 90-second run delivered both Enter events, reached a real 320x200
non-black canvas, crossed `Cache size increased by 1024 to new max of 2048
entries` at 86.4664s, and continued rendering at 56--58 FPS afterward with no
`UNIMPLEMENTED opcode`, `FATAL`, `RuntimeError`, or hang.

## 2026-09-02 20:42 IDT: browser columns plus libdivide fast path

Observation: the canonical URL
`http://localhost:8799/?WASM_TPUT=1&build=columns-libdiv` served JS
`cd92367247fdcfb9dc9e8644cdf32ca9e8bccfd5cebc80355c3ca13d8a6e5973`, WASM
`eead580e3a31591e5b028fd19c17b50f788d1c39babec6b29c542d4ced20d1e4`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index `623fafa969f1dfbb819d5ceb7eac013ae802d52ff394c0c4e464ddbb8da479e4`.
The Wine source tree remains dirty because preserved build artifacts are
untracked; no sibling checkout was modified.

Observation: the fresh 90-second CDP run delivered both Enter key pairs and
reached the active scene. Late samples were 73.8--75.9 FPS, compared with
roughly 62 FPS for the columns-only candidate. The logs showed
`Cache size increased by 1024 to new max of 2048 entries` at 85.3349s, but no
`UNIMPLEMENTED opcode`, `FATAL`, `RuntimeError`, or black-frame report before
the harness closed; the final `initial thread run returned (eip=000000e9)` was
at harness shutdown. The canvas was rendering a non-black 320x200 frame before
shutdown.

Decision: enable the pure/cache-based libdivide native path by default in the
browser, with `WASM_NO_FAST_LIBDIV=1` as its rollback switch. The verified
four-column mapper hooks remain enabled by default; other self-modifying
renderer shortcuts remain opt-in through `WASM_FAST_RENDER=1`.

## 2026-09-02 20:25 IDT: browser self-modifying renderer shortcuts disabled by default

Observation: after the late-menu reproduction reached the former 78%/cache
boundary, the guest reported `UNIMPLEMENTED opcode 06 at eip=03882816` with
bytes `06 07 08 09 0a`, followed by `initial thread run returned`. The address
is in the relocatable compiled-CON arena rather than the executable image.

Change: removed the speculative stale-return recovery and kept the guest
`cache1d::ageBlocks` implementation active. Browser-native mapper, surface-blit,
libdivide, and qrhline shortcuts are now opt-in with `WASM_FAST_RENDER=1`; the
default browser path keeps these self-modifying seams interpreted. The patched
bundle is served at `http://localhost:8799/?WASM_TPUT=1&build=smc-safe` and port
8806.
Current served hashes are JS
`cd92367247fdcfb9dc9e8644cdf32ca9e8bccfd5cebc80355c3ca13d8a6e5973`, WASM
`eead580e3a31591e5b028fd19c17b50f788d1c39babec6b29c542d4ced20d1e4`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index `623fafa969f1dfbb819d5ceb7eac013ae802d52ff394c0c4e464ddbb8da479e4`.
The Wine source tree remains dirty due to preserved build artifacts; no sibling
checkout was modified. The new late-level run is the decisive verification still
in progress; observations must not be confused with the earlier speculative
recovery candidate.

## 2026-09-02 01:40 IDT: browser JIT relocation crash removed

Observation: the canonical URL
`http://localhost:8799/?WASM_TPUT=1` now serves JS
`c027014ffdceeeea5c914536027013b8f6418f9f2c638765b351cacfb60990dc`, WASM
`8dea06047306fd3fdb531b429a8d2d59b388dc75a7995669e0406401c076e838`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index `623fafa969f1dfbb819d5ceb7eac013ae802d52ff394c0c4e464ddbb8da479e4`.
The source tree was dirty during testing; no sibling checkout was modified.

Observation: a fresh canonical 85-second run with the five-Enter menu/load
sequence reached a non-black 320x200 WebGL frame at 43.3s, reported
`input: ready — keys 10, mouse 2` and `audio: on 22050Hz/2ch`, and ended with
`fps: 50.0` and `frames: 2069`. It crossed the former cache-growth window
without `UNIMPLEMENTED opcode`, thread exit, `FATAL`, `RuntimeError`, or
`unreachable`.

Decision: browser AOT JIT is now opt-in with `WASM_JIT=1`; the interpreter is
the safe default across self-modifying level-code relocation. Stable native
hooks remain enabled, while `surfspan` is opt-in with `WASM_SURFSPAN=1`.
The promoted build is served on ports 8799 and 8806.

## 2026-09-02 01:17 IDT: level-load stall fixed and canonical bundle verified

Observation: at source commit `38a33a0c` plus the uncommitted fix, the exact
fresh Chrome sequence (Enter at 14, 18, 22, 28, and 35 seconds) no longer
exits at the cache-resize boundary. The canonical URL
`http://localhost:8799/?WASM_TPUT=1` served JS
`3e7fe799d9dabd99f066da8f5e110a16e84666cfc0d4a251f299d89560e551bd`, WASM
`a54f08e340791ac91b73b7f51cc6f91ceee1295d0b7ba190398f469cf5e82330`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index `623fafa969f1dfbb819d5ceb7eac013ae802d52ff394c0c4e464ddbb8da479e4`.
The source tree was dirty during the test; no sibling checkout was modified.

Observation: the run reached a real non-black 320x200 WebGL frame at 10.9s,
reported `input: ready — keys 10, mouse 2` and `audio: on 22050Hz/2ch`, and
ended at 45s with `fps: 66.7` and `frames: 1950`. Logs showed OpenGL context
creation and SDL Enter key down/up events, with no `UNIMPLEMENTED opcode`,
thread exit, `FATAL`, `RuntimeError`, or `unreachable`.

Decision: the broad generated-code dynamic-return shortcut is now opt-in via
`WASM_DYNAMIC_RET`; the normal interpreter return path is the browser default.
The self-modifying `surfspan` native hook is also opt-in via `WASM_SURFSPAN`
while its cache-resize interaction is being independently verified. The
verified browser bundle is promoted on ports 8799 and 8806.

## 2026-09-02 00:39 IDT: input/load stress run completed end to end

Observation: the canonical URL
`http://localhost:8799/?WASM_TPUT=1&WASM_BADIP=1` serves JS
`7100ac1d5eb17403526ec882b869de7bff7eb7a5547f4a9d75c19b424ec4d954`, WASM
`964c44f915a343e41b4ebd78d213848311d0b28bac3637d45efe40f4b929a3a2`, and data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`. The
source tree is dirty on `vibe` at `9789a1f0`; no sibling checkout was modified.

Observation: a 40-second fresh Chrome stress run with mouse motion, a held W
key, and Space reached a non-black 320x200 canvas, passed the temporary
texture-precache interval, and ended at 1,835 frames / 64 FPS. The page
reported `input: ready — keys 4, mouse 24`; guest logs showed W SDL key
down/up, and there were no `FATAL`, `RuntimeError`, or `unreachable` failures.

Decision: the screenshot's zero-FPS interval is finite asset loading. The
canonical build now completes that transition under active input and continues
rendering smoothly; no further runtime change is justified by the reproduced
evidence.

## 2026-09-02 00:23 IDT: long-run check distinguishes texture-load pause from input failure

Observation: the canonical URL
`http://localhost:8799/?WASM_TPUT=1&WASM_BADIP=1` still serves JS
`7100ac1d5eb17403526ec882b869de7bff7eb7a5547f4a9d75c19b424ec4d954`, WASM
`964c44f915a343e41b4ebd78d213848311d0b28bac3637d45efe40f4b929a3a2`, and data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`. The tree
is dirty on `vibe` at `3d4ee85d`; no sibling checkout was modified.

Observation: a 60-second fresh Chrome run reached a non-black 320x200 canvas,
reported `first-frame: 10.9s`, and ended at 2,982 frames / 65.8 FPS. It showed
the same temporary zero-FPS texture/cache-load interval as the user screenshot;
rendering resumed by t=20s. The run sent Enter at t=18s and Space at t=25s,
with `input: ready` and no `FATAL`, `RuntimeError`, or `unreachable`.

Decision: retain the SDL event plus keyboard-state hooks in the canonical
bundle. The screenshot is an in-game loading pause, not a permanent black
screen or a dead input loop; allow the initial load to finish before judging
controls.

## 2026-09-02 00:15 IDT: SDL keyboard-state hook added and canonical gate passed

Observation: the canonical URL
`http://localhost:8799/?WASM_TPUT=1&WASM_BADIP=1` serves JS
`7100ac1d5eb17403526ec882b869de7bff7eb7a5547f4a9d75c19b424ec4d954`, WASM
`964c44f915a343e41b4ebd78d213848311d0b28bac3637d45efe40f4b929a3a2`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index `623fafa969f1dfbb819d5ceb7eac013ae802d52ff394c0c4e464ddbb8da479e4`.
The Wine tree remains dirty on `vibe` at `307c4a95`; no sibling checkout was
modified.

Observation: the fresh canonical Chrome run logged
`native SDL_GetKeyboardState @ 006a8a80`, `wasm_input: SDL key down vk=0x57`,
and the corresponding key-up. The page reported `input: ready`, `keys 2,
mouse 2`, reached `first-frame: 11.2s`, and rendered a non-black 320x200
canvas. No `FATAL` or `RuntimeError` occurred.

Decision: maintain a guest SDL keyboard-state array in addition to SDL event
injection; key down/up now updates both paths. The new bundle is promoted on
ports 8799 and 8806.

## 2026-09-01 23:54 IDT: SDL key-symbol mapping corrected and canonical gate passed

Observation: the canonical URL
`http://localhost:8799/?WASM_TPUT=1&WASM_BADIP=1` serves JS
`7100ac1d5eb17403526ec882b869de7bff7eb7a5547f4a9d75c19b424ec4d954`, WASM
`7911ee3aef3d2d980a892a5f86454278ecaab6527cc510b0762f7437e1e45858`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index `623fafa969f1dfbb819d5ceb7eac013ae802d52ff394c0c4e464ddbb8da479e4`.
The Wine tree is dirty on `vibe` at `a2ca5099`; no sibling checkout was
modified.

Observation: a fresh canonical Chrome run reached `OpenGL context: version
3.3`, `Setting video mode 640x400 (8-bpp windowed)`, and a non-black 320x200
canvas (`hash e2251540`). It reported `input: ready`, `keys 2, mouse 2`, and
continued rendering at 35–1180 FPS in the sampled intervals. No `FATAL`,
`RuntimeError`, or `unreachable` occurred.

Decision: SDL keyboard symbols now use lowercase ASCII for printable keys and
SDL scancode symbols for navigation/function keys. The updated bundle is
served for testing at 8799 and 8806.

## 2026-09-01 23:34 IDT: input queue fix promoted and canonical browser gate passed

Observation: the canonical server at
`http://localhost:8799/?WASM_TPUT=1&WASM_BADIP=1` now serves JS
`7100ac1d5eb17403526ec882b869de7bff7eb7a5547f4a9d75c19b424ec4d954`, WASM
`7aa56f9e214530afc4fdee6a25671b475953d3dbbb5205b3ee7a5b86716829d2`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index `623fafa969f1dfbb819d5ceb7eac013ae802d52ff394c0c4e464ddbb8da479e4`.
The Wine tree remains dirty on `vibe` at `ad816e0a`; existing unfinished and
untracked work was preserved, and no sibling checkout was modified.

Observation: a fresh canonical Chrome run reached `OpenGL context: version
3.3`, switched to `640x400 (8-bpp windowed)`, produced a non-black 320x200
canvas (`hash e2251540` at the later capture), and reported `first-frame:
13.6s` in the verified run. The page reported `input: ready`; the test sent a
held `KeyW`, a `Space`, and mouse events (`keys 2, mouse 2`). No `FATAL`,
`RuntimeError`, or `unreachable` occurred.

Decision: the browser input ring is now bridged directly into SDL keyboard,
absolute mouse, and button events consumed by the native `SDL_PollEvent` hook;
the old Win32 message post remains as a compatibility path. The canonical
bundle is ready to test. Allow roughly 15 seconds for the first frame after a
hard reload, then click the canvas before using keys or mouse.

## 2026-09-01 22:53 IDT: optimized browser bundle promoted and rechecked

Observation: the compatibility browser build was served at
`http://localhost:8799/?WASM_TPUT=1&WASM_BADIP=1` after building with the
native mapper and runtime-return shortcut enabled. The promoted hashes are JS
`18bb2ebf0eb9b8e7752f55dbd6d74b7908d3151fe2ac137f26a6178e0de5a193`, WASM
`6a3794a1573d2c51f2f603fb6a2eabe043761c8240d94a16e346995479aab7d9`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index `40fc6f60caa2e1efd0e019dbc8d4aaafcefeedde1cb8ccddef4029197106a601`.
The Wine tree remains dirty on `vibe` at `ad816e0a`; no sibling checkout was
modified.

Observation: the final fresh Chrome run reported `input: ready`, initialized
OpenGL 3.3, switched to `640x400 (8-bpp windowed)`, produced a non-black
320x200 canvas (capture hash `78719738`), and continued to 10,736 flips.
No `FATAL`, `RuntimeError`, or `input: DISABLED` appeared. The test sent a
Space key event at 18 seconds.

Decision: promote the verified bundle to the 8799/8806 serving root. Browser
build defaults now use `CINT=-O1`, `LOPT=-O2`, and `XOPT=-O2`; this reproduces
the working browser configuration while node/support objects remain `-O3`.

Hypothesis: the earlier black-screen abort was optimizer-sensitive native
`mvlineasm1` code under the `-O3` browser link, not the runtime-return shortcut.
The compatibility build keeps both optimizations enabled and passes the real
frame/input gate; a longer differential run is still appropriate before
claiming more than this verified browser improvement.

## 2026-09-01 22:45 IDT: canonical 8799 black-screen report rechecked

Observation: the exact URL `http://localhost:8799/?WASM_TPUT=1&WASM_BADIP=1`
served the verified canonical artifacts: JS
`31b8d1395cd45cad8fadcefe986ff2d3f947b72de50897a18467813a655caac8`, WASM
`dea5dc34ca95601265e0a5a1e8ad3964949670e9c3c43f5451d2f3b9cf8640dc`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index `40fc6f60caa2e1efd0e019dbc8d4aaafcefeedde1cb8ccddef4029197106a601`.
The Wine tree is still dirty on `vibe` at `ad816e0a`; no sibling checkout was
modified.

Observation: a fresh Chrome run reported `input: ready`,
`OpenGL context: version 3.3`, `Setting video mode 640x400 (8-bpp windowed)`,
and a non-black 320x200 canvas with `first-frame: 8.2s`; no `FATAL` or
`RuntimeError` occurred. The initial black canvas is the normal game/script
startup interval.

Decision: keep 8799/8806 on the verified bundle. Do not promote the isolated
new AOT build: it aborts before video mode, including with its return shortcut
disabled. Use a fresh tab or hard reload and leave it open for roughly 10
seconds before judging the screen.

## 2026-09-01 22:05 IDT: native generated divide helper plus RGBA backpressure passed browser gate

Observation: the isolated candidate was served at
`http://localhost:8810/?WASM_TPUT=1&WASM_BADIP=1` from `/tmp/webwine-div64e`.
Candidate hashes are JS
`31b8d1395cd45cad8fadcefe986ff2d3f947b72de50897a18467813a655caac8`, WASM
`dea5dc34ca95601265e0a5a1e8ad3964949670e9c3c43f5451d2f3b9cf8640dc`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index `40fc6f60caa2e1efd0e019dbc8d4aaafcefeedde1cb8ccddef4029197106a601`.
The source tree remains dirty on `vibe` at `ad816e0a`; unrelated work was
preserved.

Observation: the candidate logged `OpenGL context: version 3.3`, switched to
`640x400 (8-bpp windowed)`, reached a non-black 320x200 canvas with
`input: ready`, and continued past the heavy render interval. The page-side
RGBA queue stayed bounded after adding two-frame shared-control backpressure;
the prior `Array buffer allocation failed` did not recur. Native traces showed
`DIV_ARM target=0080da90` and `DIV_HIT target=0080da90`. No `FATAL`,
`RuntimeError`, `JITBAD`, `JITBADEIP`, or `UNIMPLEMENTED` appeared.

Decision: promote the candidate bundle to the canonical 8799/8806 roots. The
rollback switch is `WASM_NO_NATIVE_DIV64=1`; the browser transport safeguard
is enabled unconditionally.

## 2026-09-01 22:12 IDT: canonical post-promotion recheck

Observation: the exact canonical URL
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1` served JS
`31b8d1395cd45cad8fadcefe986ff2d3f947b72de50897a18467813a655caac8`, WASM
`dea5dc34ca95601265e0a5a1e8ad3964949670e9c3c43f5451d2f3b9cf8640dc`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index `40fc6f60caa2e1efd0e019dbc8d4aaafcefeedde1cb8ccddef4029197106a601`.
The dirty Wine tree remains on `vibe` at `ad816e0a`; no sibling checkout was
modified.

Observation: Chrome logged `wasm_x86: JIT 170758 translated blocks loaded`,
`OpenGL context: version 3.3`, and `Setting video mode 640x400 (8-bpp
windowed)`. The canvas was non-black at 320x200 (`a7610394`), `input: ready`
was reported and keyboard/mouse probes were accepted. No `FATAL`,
`RuntimeError`, `JITBAD`, `JITBADEIP`, or `UNIMPLEMENTED` appeared during the
35-second recheck. The display-side rate remained transport-limited, so this
is a rendering/input and stability verification, not a claim of uncapped FPS.

## 2026-09-01 21:14 IDT: runtime-learned divide shortcut not promoted

Observation: the runtime-learned `WASM_NATIVE_DIV64=1` node candidate was
isolated in `/tmp/webwine-div64d`. It did not reach a rendered frame during a
34-second run (the guest was still compiling/relocating generated code), and
the shortcut produced no native-hit evidence. The source tree remains dirty
on `vibe` at `ad816e0a`; unrelated work was preserved.

Decision: the shortcut and external-wrapper extension were removed from the
source path. The canonical browser artifacts and their exact URL were not
changed. Only the opt-in generated-code trace/dump remains for a future
dynamic-block implementation; no FPS claim is made for this candidate.

## 2026-09-01 21:05 IDT: runtime divide-helper candidate rejected

Observation: the generated-code trace consistently identified the call edge
`0x008066f0 -> 0x0080da90`; the callee is a long signed 64-bit divide/modulo
routine and the wrapper/epilogue repeats during the renderer load. A node-only
candidate attempted to replace it with host 64-bit arithmetic under the
opt-in `WASM_NATIVE_DIV64=1` switch. The source tree remains dirty on `vibe`
at `ad816e0a`; unrelated work was preserved.

Observation: a broad byte-signature matcher falsely matched another generated
function and trapped with `RuntimeError: memory access out of bounds` before
the first frame. The exact-address candidate did not produce a measurable
native-hit or FPS improvement in its matched run. No browser artifact was
replaced and the native divide shortcut was removed; the default interpreter
path remains intact. The opt-in dynamic trace remains available for the next
runtime-generated-block investigation.

Decision: do not promote this shortcut. Any future implementation must learn
the callee from the verified veneer and validate the complete generated
function/ABI, not only a shared prologue.

## 2026-09-01 20:29 IDT: black-screen report reproduced and cleared

Observation: a fresh Chrome run against the exact canonical URL
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1` loaded JS
`d3e1cb115819a976e4cf77c2a57b9448215cde931e64f0e649059131b698ab02`, WASM
`82a97aeb4195c3def07687399e2bc6f0bfe00aa562a09541b639f9c8f695b4bf`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index `54d81676870b356aa800b9dd44bf8c27276f886edf8bb4bb8da80f85dafdf571`.
The source tree remains dirty on `vibe` at `ad816e0a`; unrelated work was
preserved.

Observation: the canvas was black with zero uploads during the initial boot,
then logged `Setting video mode 1024x768 (32-bpp windowed)`,
`OpenGL context: version 3.3`, `Setting video mode 640x400 (8-bpp windowed)`,
and reached `flips=684` at 12.9 seconds. The screenshot hash was `e2251540`
and the canvas was non-black; no `FATAL`, `RuntimeError`, `JITBAD`, or
`JITBADEIP` appeared.

Decision: the served runtime is healthy; the black interval is first-boot
JIT/game startup timing. Use the exact URL above in a fresh tab or hard-reload
the existing tab and leave it open through the first frame. Do not use the
old experimental GL-present URL.

## 2026-09-01 19:27 IDT: fresh canonical startup timing check

Observation: a fresh Chrome session at the exact URL
`http://localhost:8799/?WASM_TPUT=1&WASM_BADIP=1` loaded the canonical
artifacts (JS `d3e1cb115819a976e4cf77c2a57b9448215cde931e64f0e649059131b698ab02`,
WASM `82a97aeb4195c3def07687399e2bc6f0bfe00aa562a09541b639f9c8f695b4bf`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, index
`54d81676870b356aa800b9dd44bf8c27276f886edf8bb4bb8da80f85dafdf571`). The
source tree remains dirty on `vibe` at `ad816e0a`; unrelated work was
preserved.

Observation: the canvas remained black during the initial 12-second capture
while the guest compiled its scripts. The log then reached `Setting video mode
640x400 (8-bpp windowed)`, `OpenGL context: version 3.3`, and `flips=764` at
15.7 seconds, with no `FATAL`, `RuntimeError`, `JITBAD`, or `JITBADEIP`. This is
a slow first-frame window, not a worker or GL failure; the prior headful check
also captured the non-black 3D Realms frame and accepted keyboard input.

Decision: the rejected 8810 experiment is stopped and the canonical 8799/8806
roots were not replaced. Keep the loading overlay visible until the first
frame and use the exact URL above; an already-open tab must be fully reloaded.

## 2026-09-01 18:58 IDT: canonical headful black-screen check passed

Observation: fresh headless and headful Chrome runs against the exact URLs
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1` and
`http://localhost:8799/?WASM_TPUT=1&WASM_BADIP=1` loaded the same served bundle.
The source tree remains dirty on `vibe` at `ad816e0a`; unrelated work was
preserved. The actual served artifact hashes are JS
`d3e1cb115819a976e4cf77c2a57b9448215cde931e64f0e649059131b698ab02`, WASM
`82a97aeb4195c3def07687399e2bc6f0bfe00aa562a09541b639f9c8f695b4bf`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index.html `54d81676870b356aa800b9dd44bf8c27276f886edf8bb4bb8da80f85dafdf571`.

Observation: both runs logged `input: ready`, `GFX| OpenGL context: version
3.3`, `Setting video mode 640x400 (8-bpp windowed)`, and then real frame
counts (headful: 1,734 flips at 7.5s and 10,293 at 11.6s). The headful
screenshot showed the 3D Realms intro, so the canvas was non-black and the
keyboard input probe was accepted. No `FATAL`, `RuntimeError`, `JITBAD`, or
`JITBADEIP` appeared. The later `fps: 0` is a guest render stall after the
already-drawn frame, not a black-screen startup failure.

Decision: no Wine/runtime patch was promoted from this check because the
canonical bundle reproduces correctly in a fresh visible browser. Use a fresh
tab and the exact URL above; if an existing tab is black, hard-reload it so
the page and worker bootstrap are recreated together. The isolated 8810
chain-probe candidate remains rejected and must not be used.

## 2026-09-01 18:50 IDT: chain-probe candidate browser regression confirmed

Observation: the isolated candidate was served at
`http://localhost:8810/?WASM_TPUT=1&WASM_BADIP=1&WASM_TRACE_CHAIN_HOOK=1`
from a separate root. Its artifacts were JS
`71e436ffcd98b0ec59429e49949697345c66f663a1f6b519d6152c02938cd72d`, WASM
`c44f38bf05024a71b8f7c7ee60c68cdc12fc27befd1706c0ed284e88134e49b0`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index.html `54d81676870b356aa800b9dd44bf8c27276f886edf8bb4bb8da80f85dafdf571`.
The source tree remains dirty on `vibe` at `ad816e0a`; unrelated work was
preserved.

Observation: fresh Chrome reached `input: ready`, but after sound precache the
candidate logged `Aborted(Assertion failed)` and
`FATAL worker exception: Aborted(Assertion failed)`, with zero frames and a
black canvas. This reproduces the earlier chain-probe failure; the diagnostic
trace did not identify a safe removable boundary.

Decision: the probe-removal candidate is rejected and was never promoted. The
canonical 8806/8799 roots remain on the verified hashes recorded below.

## 2026-09-01 18:47 IDT: rejected chain-probe removal at browser gate

Observation: the proposed removal of the per-block native-hook probe passed
the short Node A/B and produced identical first-frame PPMs
(`182f29b7043a41d46a17e83529ab9ace94f534e697b046b2c943a0deb960b5ea`), with a
roughly 1% warm-FPS signal. However, the isolated browser build failed its
authoritative gate after sound precache with `FATAL worker exception: Aborted
(Assertion failed)` and zero frames. The candidate was never copied to the
canonical server.

Decision: the probe removal was reverted. The native-hook check remains in the
translated chain until the generator/runtime contract is strengthened enough
to prove the browser path, rather than relying on the short Node result.

## 2026-09-01 18:35 IDT: rejected larger AOT lookup cache

Observation: a same-source node A/B with the direct-mapped generated-block
lookup cache enlarged from 1,024 to 4,096 slots completed cleanly and retained
the normal native mapper counters. Warm samples were approximately `671.8 FPS`
for the 1,024-slot control and `663.9 FPS` for the 4,096-slot candidate; the
candidate also showed no stable improvement in `kinsn/frame`.

Decision: the cache enlargement was reverted. The canonical browser artifact
was not rebuilt or changed, and the default cache remains 1,024 slots.

## 2026-09-01 18:27 IDT: runtime-renderer AOT candidate stopped before promotion

Observation: the steady-frame profile's `0x00805bd7` entry is a one-byte
`ret` trampoline and accounts for one interpreted entry per frame; translating
the surrounding `0x805000-0x806000` range produced 257 integer-split blocks,
but this is not a large instruction hotspot by itself.

Observation: an isolated `WASM_RUNTIME_AOT=1` node build loaded the 257-block
candidate, but the final link did not produce a complete candidate JavaScript
launcher. Pairing the partial WASM artifact with the control launcher then
stalled before the first frame, so it is not valid promotion evidence. The
candidate was removed from the source/build path; the canonical browser bundle
was not changed.

Decision: do not pursue the `0x805bd7` trampoline as an FPS optimization. The
remaining work returns to a real multi-instruction renderer hotspot rather
than a once-per-frame return boundary.

## 2026-09-01 18:19 IDT: canonical browser rechecked after black-screen report

Observation: a fresh Chrome run against the exact canonical URL
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1` loaded the served hashes already
recorded below, reached `input: ready`, created an OpenGL 3.3 context, and drew
a non-black frame at 320x200 (canvas backing size 640x400 before the game mode
switch). The screenshot hash at 10 seconds was `b18916a1`; the frame visibly
contained the 3D Realms intro. No `FATAL`, `RuntimeError`, `JITBAD`, or
`JITBADEIP` appeared. The first frame arrived at about 7.3 seconds; the later
reported `fps: 0` was a render stall while the already-drawn intro remained on
screen, not an all-black framebuffer.

Observation: the server returned `Cache-Control: no-store` plus both required
cross-origin isolation headers. The page and worker also append a fresh boot
query to every worker/WASM/data request, so a fresh tab is using a matched
bundle. The source tree remains dirty on `vibe`; unrelated work is preserved.

Action: use the exact URL above in a fresh tab (or hard-reload the existing tab,
then wait through the roughly 7–10 second first boot). If that tab still shows
black, append `&beacon=1` and capture the visible `worker:`/`FATAL` lines from
the page log; that distinguishes a local browser/extension failure from a Wine
render failure.

## 2026-09-01 18:15 IDT: fresh frame profile and exact BitBlt AOT rejection

The canonical served artifact remains JS
`d3e1cb115819a976e4cf77c2a57b9448215cde931e64e0f649059131b698ab02`, WASM
`7be04e487b96146ea75ae993dd2b7fd80fa37190e8e11add03fb97221b045486`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index.html `54d81676870b356aa800b9dd44bf8c27276f886edf8bb4bb8da80f85dafdf571`.
It is served at
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the source tree is dirty
on `vibe` at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`, with unrelated work
preserved.

Observation: a fresh `PROFILE=1` frame-scoped node run confirmed the earlier
shape. Before the first real frame, GDI pages `3e3b0000` and `3e3f0000` were
55.3% and 29.7% of interpreted entries; after rendering began they fell to
43.0% and 23.1%, while the executable renderer page `0x630000` rose to 5.4%.
The exact GDI misses were `3e3bbd56` (BitBlt body) and `3e3fac2a`
(`get_gdi_client_ptr` body), each once per frame in the sampled interval.

Observation: an exact 28-block relocatable BitBlt table covering
`0x1000bc70-0x1000be20` loaded and rendered without `FATAL`, `RuntimeError`,
`JITBAD`, `JITBADEIP`, or `UNIMPLEMENTED`, but its matched node sample was
`884.4 FPS` versus `883.8 FPS` control and is therefore neutral. The generated
table was restored to the prior narrow WidenPath candidate; it is not served.

Hypothesis: the remaining GDI share is wrapper/handle traffic whose interpreter
cost is too small or too irregular for a static table to move steady FPS. The
next useful optimization must target the executable renderer page or replace
the runtime block dispatch itself, not broaden GDI speculation.

## 2026-09-01 17:26 IDT: GDI client-lookup candidate verified, not promoted

The source tree is dirty on `vibe` at
`ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`; unrelated changes were preserved.
The rebuilt bundle is served at
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the opt-in candidate URL is
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1&WASM_GDI_CLIENT=1`. Artifact
hashes are JS
`d3e1cb115819a976e4cf77c2a57b9448215cde931e64e0f649059131b698ab02`, WASM
`7be04e487b96146ea75ae993dd2b7fd80fa37190e8e11add03fb97221b045486`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index.html `54d81676870b356aa800b9dd44bf8c27276f886edf8bb4bb8da80f85dafdf571`.

Observation: the candidate native hook for `gdi32!get_gdi_client_ptr`
(RVA `0x4abc1`) passed the TEB64 path, stack/return ABI checks, and a node
control/candidate frame capture with identical PPM hash
`182f29b7043a41d46a17e83529ab9ace94f534e697b046b2c943a0deb960b5ea`.
Fresh browser control and candidate runs both reached a non-black 640x400
canvas (CDP frame hash `e2251540`), `input: ready`, and OpenGL 3.3, with no
`FATAL`, `RuntimeError`, `JITBAD`, `JITBADEIP`, or `UNIMPLEMENTED` output.

Observation: matched node warm samples were effectively neutral. One pair
was about 716 vs 727 FPS, but a reversed pair was 669 vs 670 FPS and the
`kinsn/frame` values were 215 vs 216. The candidate remains disabled by
default; enable it only with `WASM_GDI_CLIENT=1` while profiling.

Hypothesis: GDI handle lookup is prominent during startup and some render
stalls, but is not a stable steady-state FPS bottleneck. Continue toward the
remaining software-renderer hotspot rather than promoting this neutral hook.

The focused relocatable AOT control for the same helper (`31` blocks covering
`0x1004abc0-0x1004ace0`, enabled with `WASM_GDI32_JIT=1`) was also rejected:
it rendered without errors but raised warm node load to about `290 mips` from
about `250 mips`. `webwine/gdi32_gen_blocks.c` was restored to the prior
8-block WidenPath candidate; the focused table is not in the served bundle.

## 2026-09-01 17:04 IDT: fresh canonical black-screen reproduction

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; port 8799 serves the same
bundle. The source tree is dirty on `vibe` at
`ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`; unrelated changes were preserved.
The served artifact hashes are JS
`d3e1cb115819a976e4cf77c2a57b9448215cde931e64e0f649059131b698ab02`, WASM
`a4b7368dd72c00e42856fc34f0f4dac74a23cce82cf76a18ec8b4312664a5e00`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index.html `54d81676870b356aa800b9dd44bf8c27276f886edf8bb4bb8da80f85dafdf571`.

Observation: fresh headless Chrome runs against both the exact canonical URL
and `/` reached `GFX| OpenGL context: version 3.3`, executed `autoexec.cfg`,
and posted a non-black 640x400 canvas with screenshot hash `e2251540`.
The first frame arrived at about 6.1 seconds; the startup overlay remained
visible until that frame. No `FATAL`, `RuntimeError`, `JITBAD`, or `JITBADEIP`
was observed. A later long guest-render interval can report `fps: 0` while
retaining the already-rendered image; this is a stall, not a black framebuffer.

Hypothesis: a user's persistent black tab is likely an old tab/cache or a
different server root rather than the current canonical artifact. Hard reload
the exact URL above and wait for the `input: ready` / first-frame transition.

## 2026-09-01 16:10 IDT: full FP hot-range candidate measured

The canonical bundle remains served at
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; port 8799 remains the
compatibility URL. The source tree is dirty on `vibe` at
`ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`; unrelated changes were preserved.
The canonical artifact hashes remain JS
`d3e1cb115819a976e4cf77c2a57b9448215cde931e64e0f649059131b698ab02`, WASM
`82a97aeb4195c3def07687399e2bc6f0bfe00aa562a09541b639f9c8f695b4bf`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index.html `54d81676870b356aa800b9dd44bf8c27276f886edf8bb4bb8da80f85dafdf571`.

Observation: the full opt-in FP table (`WASM_FP_HOT=1`) linked with normal
`-O3` browser optimization reached a real non-black `320x200` frame with
screenshot hash `e2251540`, continuous flips, and no fatal/runtime/JIT errors.
The measured interval was about `2004 FPS`, versus roughly `2050–2200 FPS`
for matched controls, so this is not a win. The earlier `-O0` browser link
aborted during startup and is not a valid performance configuration.

Change: added opt-in FP-table differential checking (`WASM_FP_HOT_VERIFY=1`)
and a selectable generation range. The optimized node verifier reached the
game without reporting a FP mismatch before timeout; the candidate still
does not meet the evidence needed for promotion. No FP table was served or
enabled by default. `git diff --check` passed.

Hypothesis: the remaining GDI relay-page share is largely fixed thunk traffic,
not the frame limiter; the measured native relay shortcut was neutral. Keep
the verified default and continue profiling the software-render call path.

## 2026-09-01 16:02 IDT: isolated FP table browser test

The canonical served bundle was not changed. The source tree remains dirty on
`vibe` at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`, with unrelated changes
preserved. The verified bundle hashes remain JS
`d3e1cb115819a976e4cf77c2a57b9448215cde931e64e0f649059131b698ab02`, WASM
`82a97aeb4195c3def07687399e2bc6f0bfe00aa562a09541b639f9c8f695b4bf`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index.html `54d81676870b356aa800b9dd44bf8c27276f886edf8bb4bb8da80f85dafdf571`.

Observation: a broad FP candidate aborted in the browser before GL creation;
the exact five-block `0x53b9c8` candidate also aborted when linked at `-O0`.
Rebuilding that same candidate with the normal `-O3` browser link rendered a
real `320x200` non-black frame (`e2251540`) and reported continuous flips, but
its short run was about `2046 FPS`, within noise of the verified bundle and not
a measured win. No candidate was served on ports 8806 or 8799.

Change: `FP_HOT` is now an opt-in, range-selectable experiment; the generator
no longer inherits unrelated explicit entry roots, which reduced the exact
candidate to 5 blocks. It remains disabled in all normal builds.

Hypothesis: the FP table needs per-block differential verification and a
properly optimized browser link before it can be considered safe or useful.
The next performance work should return to a measured native/GDI or software
render hotspot rather than promote this candidate.

## 2026-09-01 15:35 IDT: served black-screen report reproduced as healthy

The exact URLs tested were
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1` and
`http://localhost:8799/?WASM_TPUT=1&WASM_BADIP=1`. The source tree remains
dirty on `vibe` at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`; unrelated work
was preserved. Both servers returned the same verified artifact hashes: JS
`d3e1cb115819a976e4cf77c2a57b9448215cde931e64e0f649059131b698ab02`, WASM
`82a97aeb4195c3def07687399e2bc6f0bfe00aa562a09541b639f9c8f695b4bf`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index.html `54d81676870b356aa800b9dd44bf8c27276f886edf8bb4bb8da80f85dafdf571`.

Observation: fresh headless Chrome runs reached `GFX| OpenGL context: version
3.3`, then `GFX| Setting video mode 640x400 (8-bpp windowed)`, and reported
`FPSSAMPLE ... flips=8974` on port 8806 and `flips=8974` on port 8799. The
canvas became `320x200`; screenshot `shot-010s.png` was non-black with hash
`e2251540`. No `RuntimeError`, `FATAL`, `JITBAD`, `JITBADEIP`, or unknown
native-transfer line occurred. The node setup-hook candidate also reached the
game and rendered frames, but is not the served bundle.

Hypothesis: a black screen seen in an existing tab is stale page/worker state
or the wrong server root, not a failure in the current verified artifact. Use
the explicit URLs above and a hard reload; do not use a bare `/` URL.

Change: the new mapper setup hook was corrected so `prosetupvlineasm` has its
own enum value, was wired into initialization, and passed `git diff --check`.
It remains opt-in (`WASM_SETUP_MAPPERS=1`) and has not replaced the verified
browser bundle.

## 2026-09-01 15:28 IDT: relay hook link-pressure workaround measured

The verified browser bundle remains served at
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1` and the compatibility URL
`http://localhost:8799/?WASM_TPUT=1&WASM_BADIP=1`; both servers are live. The
source tree remains dirty on `vibe` at
`ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`, with unrelated changes preserved.
The served hashes remain JS
`d3e1cb115819a976e4cf77c2a57b9448215cde931e64e0f649059131b698ab02`, WASM
`82a97aeb4195c3def07687399e2bc6f0bfe00aa562a09541b639f9c8f695b4bf`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index.html
`54d81676870b356aa800b9dd44bf8c27276f886edf8bb4bb8da80f85dafdf571`.

Observation: with optimized object compilation and a low-memory final link,
matched node runs of the opt-in `WASM_GDI32_RELAY=1` candidate and its control
both rendered continuously without `FATAL`, `RuntimeError`, `JITBAD`,
`JITBADEIP`, or unknown-transfer errors. Warm intervals were approximately
`929.8 FPS` candidate versus `932.6 FPS` control; this is neutral within run
noise, so the relay hook remains opt-in and is not promoted. The canonical
browser bundle was not replaced. `git diff --check` passed.

Change: `build-node.sh` now accepts `LINKOPT` independently from compile
optimization; its default remains `LINKOPT=$OPT`. This permits large isolated
experiments to link without changing the shipped optimized configuration.

Hypothesis: removing the GDI relay interpreter entries alone is not a material
FPS lever in this scene. The next target should be the remaining executable
software-render path or a measured SDL/ntdll call cluster, with the relay hook
kept as a correctness-checked rollback control.

## 2026-09-01 15:24 IDT: isolated Wine relay optimization measured

The canonical test URL was
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the same verified bundle
was also available at port 8799. The source tree remains dirty on `vibe` at
`ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`; unrelated changes were preserved.
The served artifact SHA-256 values remain JS
`d3e1cb115819a976e4cf77c2a57b9448215cde931e64e0f649059131b698ab02`, WASM
`82a97aeb4195c3def07687399e2bc6f0bfe00aa562a09541b639f9c8f695b4bf`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index.html
`54d81676870b356aa800b9dd44bf8c27276f886edf8bb4bb8da80f85dafdf571`.

Observation: a fresh frame-scoped browser profile reached a real 320x200
canvas (`e2251540`) and reported the render-time residue as
`3e3f0000=31.7% 007a0000=19.0% 00510000=12.7% 00730000=12.7%
006f0000=9.9%`; no fatal, runtime, JIT, or NULL-transfer error occurred.
The exact `3e3f` code is the GDI32 generated relay-thunk cluster, not the
WidenPath implementation body.

Change: added an opt-in `WASM_GDI32_RELAY=1` shortcut that byte-verifies and
arms 404 of 495 GDI32 relay thunks, preserving the original descriptor/index
stack layout and handing control to the same native relay trampoline. The
optimized default build could not be linked within the local resource limit
after this source addition; an `OPT=-O0` matched node probe booted and rendered
but is not a valid FPS comparison. The canonical served bundle was not
replaced, and the option remains disabled by default. `git diff --check` passed.

Hypothesis: the relay cluster is a real interpreter hotspot, but it needs a
normal optimized bundle A/B before promotion. The next implementation step is
to reduce compile/link pressure or move this narrow dispatcher shortcut into a
separately compiled unit, then repeat the matched browser measurement.

## 2026-09-01 14:57 IDT: restored the URL from the handoff

The previously advertised URL `http://localhost:8799/?WASM_TPUT=1&WASM_BADIP=1`
had no listening server, so a browser opened there could only show a blank
page. Port 8799 now serves the same verified bundle as the canonical port 8806.
The source tree remains dirty on `vibe` at
`ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`; unrelated changes were preserved.
The served artifact SHA-256 values are JS
`d3e1cb115819a976e4cf77c2a57b9448215cde931e64e0f649059131b698ab02`, WASM
`82a97aeb4195c3def07687399e2bc6f0bfe00aa562a09541b639f9c8f695b4bf`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index.html
`54d81676870b356aa800b9dd44bf8c27276f886edf8bb4bb8da80f85dafdf571`.

Observation: a fresh CDP run against port 8799 reached the real 640x400
canvas, accepted input, produced the non-black screenshot hash `e2251540`,
and logged sustained frames including
`FPSSAMPLE t=10.4 flips=9384 fps=2081.1`. No `FATAL`, `RuntimeError`,
`JITBAD`, `JITBADEIP`, or NULL-transfer error appeared.

Change: the unsafe full-GDI CFG experiment was discarded after its node safety
test transferred to unknown native address `0000002c` before startup. The
source generator is back to the narrow, opt-in eight-block WidenPath candidate;
the verified served bundle was not replaced. `git diff --check` passed.

## 2026-09-01 14:49 IDT: GDI32 WidenPath AOT candidate tested

The normal served URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`, with the verified bundle
unchanged. A separate candidate was served at
`http://localhost:8807/?WASM_TPUT=1&WASM_BADIP=1&WASM_GDI32_JIT=1`; it uses the
same dirty `vibe` tree at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`, with
unrelated changes preserved. Candidate hashes are JS
`1d984f99ae5ec0dcb444362a429d32b33a6d00814e2010cc380c26ecfab4da0b`, WASM
`f89b9953e8db54a5ecbb096e22770c04a2da7e2a939a9bf266f10829ef97c033`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index.html
`54d81676870b356aa800b9dd44bf8c27276f886edf8bb4bb8da80f85dafdf571`.

Observation: the candidate loaded `gdi32 JIT 8 translated blocks loaded`,
reached the real 320x200 canvas (`e2251540`), and sustained roughly
`2,070–2,210 FPS` in the frame intervals; the no-candidate control reached
roughly `2,068–2,334 FPS` and the same canvas hash. Neither run emitted
`FATAL`, `RuntimeError`, `JITBAD`, or `JITBADEIP`. The candidate is retained as
opt-in infrastructure but is not promoted as a default FPS gain because this
short run shows no measurable improvement and its generated blocks still
single-step four unsupported instructions. `git diff --check` passed.

Hypothesis: GDI32 `WidenPath` is present in the sampled residue but is not
large enough, or not frequent enough in this scene, to move total frame time;
the next target should be a larger measured GDI32 function or another
frame-scoped Wine call cluster.

## 2026-09-01 14:37 IDT: frame-scoped profile confirms remaining runtime hotspot

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the source tree is dirty on
`vibe` at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`, with unrelated changes
preserved. The served artifact hashes are JS
`d3e1cb115819a976e4cf77c2a57b9448215cde931e64e0f649059131b698ab02`, WASM
`82a97aeb4195c3def07687399e2bc6f0bfe00aa562a09541b639f9c8f695b4bf`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index.html
`54d81676870b356aa800b9dd44bf8c27276f886edf8bb4bb8da80f85dafdf571`.

Observation: the browser frame-scoped profile reached the real 320x200
canvas (`e2251540`). Its final sampled interval reported
`IPAGE top ... 3e3f0000=32.0% 007a0000=19.2% 00510000=12.8%
00730000=12.8% 006f0000=9.9%`; no fatal, runtime, JIT, or NULL-transfer
errors appeared. A node A/B sanity check also showed the already-shipped
surface-span native path at roughly 2,100 FPS versus roughly 750 FPS with
`WASM_NO_SURFSPAN=1`, confirming that path is not the next candidate. No new
optimization was shipped from this profile because the leading `0x3e3f` page
is a Wine-side runtime path whose exact ABI still needs symbol resolution.
`git diff --check` passed.

Hypothesis: the next meaningful gain is likely in the Wine-side `0x3e3f`
runtime/audio path, while the guest mapper and surface conversion work are
already covered by guarded native paths. Resolve its owning module and exact
function before adding a shortcut.

## 2026-09-01 14:26 IDT: fixed black-screen startup regression

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the source tree is dirty on
`vibe` at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`, with unrelated changes
preserved. The served artifact hashes are JS
`d3e1cb115819a976e4cf77c2a57b9448215cde931e64e0f649059131b698ab02`, WASM
`82a97aeb4195c3def07687399e2bc6f0bfe00aa562a09541b639f9c8f695b4bf`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index.html
`54d81676870b356aa800b9dd44bf8c27276f886edf8bb4bb8da80f85dafdf571`.

Observation: a fresh canonical run reached a real 320x200 gameplay canvas
(CSS 1024x640), produced screenshot hash `e2251540`, and reported
`FPSSAMPLE t=10.1 flips=9013 fps=2138.5`; input reached ready state and the
`w` key was accepted. No `FATAL`, `RuntimeError`, `JITBAD`, `JITBADEIP`, or
NULL-transfer error appeared. The default `pthread_getspecific` shortcut is
now opt-in (`WASM_PTHREAD_GETSPECIFIC=1`) because its default-enabled build
stalled before the first flip; the normal interpreter path is restored. The
experimental shortcut remains available for isolated testing. `git diff
--check` passed.

Hypothesis: the shortcut's direct TLS/cancellation elision changed startup
thread behavior, while the surrounding native hooks remain compatible. Keep
it opt-in until it passes a longer startup/render/input regression run.

## 2026-09-01 13:27 IDT: pthread lock/unlock pair verified

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the source tree is dirty on
`vibe` at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`, with unrelated changes
preserved. The served artifact hashes are JS
`b5ccbe12c0672697bea8b5c8963aa7a0dcab841da23d9df16a18bc91ca92ff1b`, WASM
`05810a5bb2d732b29ade18c75a02e4e05a1e4e894d8153c715e5b9314c241b1f`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index.html
`54d81676870b356aa800b9dd44bf8c27276f886edf8bb4bb8da80f85dafdf571`.

Observation: guarded native hooks armed for `_pthread_spin_unlock` at
`0x00809330` and uncontended `_pthread_spin_lock` at `0x008092e0`. The active
run reached a real 640x400 canvas (`8d3066c`) and `999.3 FPS` at `t=35.7`;
the both-hooks-disabled control reached a real canvas (`f1df6284`) and
`1000.7 FPS` at `t=35.3`. Neither run emitted `FATAL`, `RuntimeError`,
`JITBAD`, `JITBADEIP`, or NULL-transfer errors. Both hooks remain enabled as
verified low-risk reductions, without claiming a standalone FPS percentage
because the present loop is capped and timing is noisy. `git diff --check`
passed.

Hypothesis: the pair removes repeated uncontended pthread protocol overhead,
but it is not the dominant capped-frame cost. Continue toward the remaining
long mapper/dynamic intervals.

## 2026-09-01 13:21 IDT: pthread spin-unlock fast path verified

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the source tree is dirty on
`vibe` at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`, with unrelated changes
preserved. The served artifact hashes are JS
`b5ccbe12c0672697bea8b5c8963aa7a0dcab841da23d9df16a18bc91ca92ff1b`, WASM
`80681edb38d872abfa7ffc0d903e55cd03f7fe5088a9e2adbafa7d30f38050b4`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index.html
`54d81676870b356aa800b9dd44bf8c27276f886edf8bb4bb8da80f85dafdf571`.

Observation: the byte-guarded native `_pthread_spin_unlock` hook armed at
`0x00809330`; the active run reached a real 640x400 canvas (`8a03c098`) and
about `1000 FPS` after warm-up (`t=33.8`, `fps=999.4`). The opt-out control
also rendered a real canvas (`d625f6fc`) at about `1000 FPS` (`t=35.6`,
`fps=1000.2`); neither run emitted `FATAL`, `RuntimeError`, `JITBAD`,
`JITBADEIP`, or NULL-transfer errors. The hook is retained as a low-risk
instruction-boundary reduction; timing is capped/noisy, so no percentage gain
is claimed. `git diff --check` passed.

Hypothesis: this removes a repeated interpreted helper in `pthread_getspecific`,
but the browser’s present loop caps observed FPS. Continue with the remaining
long mapper/dynamic clusters.

## 2026-09-01 13:16 IDT: guarded WIN_GL_SwapWindow fast path verified

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the source tree is dirty on
`vibe` at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`, with unrelated changes
preserved. The served artifact hashes are JS
`b5ccbe12c0672697bea8b5c8963aa7a0dcab841da23d9df16a18bc91ca92ff1b`, WASM
`a9d4c0a9523e9e01d97ca5db4e73bc154568c1213d70ed352d76f592b3a3df72`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index.html
`54d81676870b356aa800b9dd44bf8c27276f886edf8bb4bb8da80f85dafdf571`.

Observation: the byte-guarded `WIN_GL_SwapWindow` hook armed at
`0x007a9fd0` and the run produced a real 640x400 canvas (`66e2cc6c`), with
sustained `1000.3 FPS` at `t=33.7`. No `FATAL`, `RuntimeError`, `JITBAD`,
`JITBADEIP`, or NULL-transfer error appeared. The hook is retained; timing is
still capped/noisy, so no isolated percentage claim is made. `git diff --check`
passed.

Hypothesis: bypassing this dynamic wrapper removes an interpreter boundary on
each swap, but the remaining long intervals are still mapper-heavy. The next
target should be the larger dynamic `0x007a`/SDL-Wine residue or mapper work.

## 2026-09-01 13:09 IDT: SDL TLS fast path active and regression-checked

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the source tree is dirty on
`vibe` at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`, with unrelated changes
preserved. The served artifact hashes are JS
`b5ccbe12c0672697bea8b5c8963aa7a0dcab841da23d9df16a18bc91ca92ff1b`, WASM
`a94479b5ac2c9b8083cec127f06d0081357c143c158435f9c08f73e6e2bb4ebe`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index.html
`54d81676870b356aa800b9dd44bf8c27276f886edf8bb4bb8da80f85dafdf571`.

Observation: the corrected byte-guarded hook logged
`native SDL_TLSGet_REAL cached path @ 006ff720`. The active run reached a real
640x400 canvas (`c4b500a4`) and sustained about `1000 FPS` after warm-up
(`t=33.6`, `fps=1000.1`); the opt-out control also reached about `1000 FPS`
(`t=34.1`, `fps=998.5`), and neither run emitted `FATAL`, `RuntimeError`,
`JITBAD`, or `JITBADEIP`. The hook is retained as a verified low-risk
optimization, without claiming a measurable standalone percentage.

Hypothesis: SDL TLS lookup is not the dominant remaining frame cost. The next
target is the larger dynamic `0x007a`/SDL-Wine call residue or the long
single-column mapper interval. `git diff --check` passed.

## 2026-09-01 13:08 IDT: SDL TLS cached lookup fast path verified

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the source tree is dirty on
`vibe` at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`, with unrelated changes
preserved. The served artifact hashes are JS
`b5ccbe12c0672697bea8b5c8963aa7a0dcab841da23d9df16a18bc91ca92ff1b`, WASM
`a94479b5ac2c9b8083cec127f06d0081357c143c158435f9c08f73e6e2bb4ebe`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index.html
`54d81676870b356aa800b9dd44bf8c27276f886edf8bb4bb8da80f85dafdf571`.

Observation: the new byte-guarded `SDL_TLSGet_REAL` cached-path hook armed at
`0x006ff720`. The active run reached sustained `~1000 FPS` after warm-up
(`FPSSAMPLE t=33.6 flips=32693`, `fps=1000.1`), with a non-black 640x400
canvas (`c4b500a4`) and no `FATAL`, `RuntimeError`, `JITBAD`, or `JITBADEIP`.
The opt-out control also reached about `1000 FPS`, so this is retained as a
verified low-risk reduction in wrapper interpretation, but no standalone FPS
percentage is claimed. `git diff --check` passed.

Hypothesis: the cached SDL TLS wrapper is not a dominant remaining frame cost;
the next measurable gain should target the larger dynamic `0x007a`/SDL-Wine
call clusters or the long single-column mapper interval.

## 2026-09-01 02:25 IDT: rejected 0x007a9fd0 candidate; verified frame restored

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the source tree is dirty on
`vibe` at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`, with unrelated changes
preserved. The served rollback artifact hashes are JS
`b5ccbe12c0672697bea8b5c8963aa7a0dcab841da23d9df16a18bc91ca92ff1b`, WASM
`e66b14b10e76951454702335a575bb497dcfd416bd0152c52bda7a3c6be3472d`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index.html
`54d81676870b356aa800b9dd44bf8c27276f886edf8bb4bb8da80f85dafdf571`.

Observation: adding generated root `0x007a9fd0` booted without a fatal error
but reached `flips=0` and screenshot hash `84db3de8` (black) through 22
seconds, so it was removed. The rebuilt rollback loaded 170,757 translated
blocks, reached `flips=777` at `t=16.1` (`620.5 FPS`), and produced the real
640x400 canvas hash `e2251540`; no `FATAL`, `RuntimeError`, `JITBAD`, or
`JITBADEIP` appeared. `git diff --check` passed.

Hypothesis: the 0x007a9fd0 static translation has a dynamic continuation or
state dependency not captured by its byte-exact local block, so it remains
unsafe to promote without a runtime-aware continuation mechanism.

## 2026-09-01 02:00 IDT: generated lock entry promoted and verified

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the source tree is dirty on
`vibe` at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`, with unrelated changes
preserved. The build recipe now explicitly seeds runtime entry
`0x0073cb20`, and the regenerated table loads 170,757 translated blocks. The
served artifact hashes are JS
`b5ccbe12c0672697bea8b5c8963aa7a0dcab841da23d9df16a18bc91ca92ff1b`, WASM
`e66b14b10e76951454702335a575bb497dcfd416bd0152c52bda7a3c6be3472d`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index.html
`54d81676870b356aa800b9dd44bf8c27276f886edf8bb4bb8da80f85dafdf571`.

Observation: the generator-backed entry passed the browser gate. The exact
frame-scoped miss report no longer contains the prior `0x0073cb20` cluster;
the clean run reached `flips=3207` at `t=19.5` (`638.3 FPS`) and then the
known long mapper interval (`flips=3390`, followed by zero while the worker
remained alive). The 640x400 canvas hash was `e2251540`; no `FATAL`,
`RuntimeError`, `JITBAD`, or `JITBADEIP` appeared. `git diff --check` passed.

Hypothesis: explicitly seeding the runtime-generated lock entry removes its
repeated interpreter prologue and preserves the existing translated retry
blocks. The next measurable residue is now the remaining `0x007a` generated
cluster and CRT/NTDLL work; do not infer a final FPS uplift from this one
timing-sensitive sample until a matched rollback is run.

## 2026-09-01 01:48 IDT: global-index lock entry rejected; verified bundle restored

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the source tree is dirty on
`vibe` at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`, with unrelated changes
preserved. The restored artifact hashes are JS
`b5ccbe12c0672697bea8b5c8963aa7a0dcab841da23d9df16a18bc91ca92ff1b`, WASM
`6a3e49c755ec1b6021009c6a75620ffdeb16c47f32a427c75ccc587831a46869`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index.html
`54d81676870b356aa800b9dd44bf8c27276f886edf8bb4bb8da80f85dafdf571`.

Observation: adding the translator-generated `0x0073cb20` entry with its full
global successor index (`148159`) still left the browser at zero flips through
24 seconds. It was removed. The final normal-build regression control emitted
the real canvas hash `e2251540`, no `FATAL`, `RuntimeError`, or `JITBAD`, and
`git diff --check` passed; no FPS gain is claimed for the rejected candidate.

Hypothesis: the live runtime-generated synchronization code cannot be safely
joined to the static AOT table solely by address and captured bytes. The next
optimization should be a runtime-aware block/continuation mechanism or the
owning Wine/SDL helper.

## 2026-09-01 01:19 IDT: complete generated-lock entry rejected; normal bundle restored

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the source tree is dirty on
`vibe` at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`, with unrelated changes
preserved. The restored bundle hashes are JS
`b5ccbe12c0672697bea8b5c8963aa7a0dcab841da23d9df16a18bc91ca92ff1b`, WASM
`6a3e49c755ec1b6021009c6a75620ffdeb16c47f32a427c75ccc587831a46869`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index.html
`54d81676870b356aa800b9dd44bf8c27276f886edf8bb4bb8da80f85dafdf571`.

Observation: a complete translated `0x0073cb20` entry (atomic exchange,
retry-limit branch, and all continuations) still produced zero frames through
24 seconds. It was removed and rebuilt out. The regression-control run on the
normal bundle reached `flips=827` at `t=14.3` (`629.5 FPS`) and a real canvas
hash `e2251540` at 640x400, with no `FATAL`, `RuntimeError`, or `JITBAD`;
`git diff --check` passed.

Hypothesis: this runtime-generated routine cannot be safely entered through the
static AOT dispatcher; its dynamic continuation or address generation differs
from the captured bytes. Keep the verified profiling instrumentation, but do
not promote a static entry. The next optimization should target the lock via
its owning Wine/SDL helper or add a runtime-aware continuation mechanism.

## 2026-09-01 00:59 IDT: rejected generated-lock AOT entry; regression restored

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_IPAGE=1&WASM_IPAGE_FRAME=1`; the
source tree is dirty on `vibe` at
`ad816e0a7dd68ee903b0b0e8d89967e82a79f325b` and unrelated changes were
preserved. The restored normal artifact hashes are JS
`b5ccbe12c0672697bea8b5c8963aa7a0dcab841da23d9df16a18bc91ca92ff1b`, WASM
`6a3e49c755ec1b6021009c6a75620ffdeb16c47f32a427c75ccc587831a46869`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index.html
`54d81676870b356aa800b9dd44bf8c27276f886edf8bb4bb8da80f85dafdf571`.

Observation: adding a translated entry for generated address `0x0073cb20`
caused the browser run to remain at zero flips through 18 seconds, so that
entry was removed. The regression-control run after rebuilding reached
`flips=849` and a real 640x400 canvas with hash `e2251540`; it emitted no
`FATAL`, `RuntimeError`, or `JITBAD`. `git diff --check` passed.

Hypothesis: the generated routine's stack/continuation is not compatible with
the static AOT chain as modeled. Its verified byte dump remains useful, but the
next implementation must handle the whole synchronization protocol (including
the helper call and retry continuation) in a dedicated guarded path.

## 2026-09-01 00:37 IDT: isolated generated synchronization hotspot

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_IPAGE=1&WASM_IPAGE_FRAME=1&WASM_IPAGE_DETAIL=1`;
the source tree is dirty on `vibe` at
`ad816e0a7dd68ee903b0b0e8d89967e82a79f325b` and unrelated changes were
preserved. The diagnostic bundle hashes are JS
`b5ccbe12c0672697bea8b5c8963aa7a0dcab841da23d9df16a18bc91ca92ff1b`, WASM
`104b9f10660c2456fa6c1587a6e2c89c2346acbf074ed2cbc0bb59381fed75e9`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index.html
`54d81676870b356aa800b9dd44bf8c27276f886edf8bb4bb8da80f85dafdf571`.

Observation: exact miss counts show the former `0052a6*` residue is gone.
The largest remaining generated-code cluster is `0x0073cb20` through its
interior entries, each about 800 hits in the frame-scoped sample. Its runtime
bytes are a verified atomic exchange/retry loop followed by a helper call at
`0x00739cc0`; the helper itself conditionally calls an indirect target and has
the lock protocol's cleanup/return sequence. The run still rendered the real
640x400 canvas and emitted no `FATAL`, `RuntimeError`, or `JITBAD`.

Hypothesis: this is a synchronization slow path, not a plain atomic getter.
The next native candidate must preserve the exchange ordering, retry behavior,
and conditional helper call; an unconditional lock elision would be unsafe.

## 2026-09-01 00:25 IDT: focused msvcrt AOT also rejected

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the source tree is dirty on
`vibe` at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b` and unrelated changes
were preserved. The live artifact was restored to the normal non-msvcrt-AOT
build. Its hashes are JS
`b5ccbe12c0672697bea8b5c8963aa7a0dcab841da23d9df16a18bc91ca92ff1b`, WASM
`104b9f10660c2456fa6c1587a6e2c89c2346acbf074ed2cbc0bb59381fed75e9`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index.html
`54d81676870b356aa800b9dd44bf8c27276f886edf8bb4bb8da80f85dafdf571`.

Observation: the smaller focused table (`MSVCRT_AOT=1 MSVCRT_AOT_RANGE=1`)
loaded 143 translated blocks and reached a real 640x400 frame (`e2251540`),
but its first measured render interval was only about 412 FPS and then hit the
same mapper stall. It was slower than the normal matched run and was not
promoted. A final normal-build smoke check still produced `e2251540`, passed
`git diff --check`, and emitted no `FATAL`, `RuntimeError`, or `JITBAD`.

Hypothesis: CRT AOT is not the next FPS lever; the overhead/coverage tradeoff
is unfavorable even for the safe focused table. Continue with runtime-generated
game-code profiling and targeted, byte-guarded helpers.

## 2026-09-01 00:06 IDT: rejected msvcrt AOT A/B; restored verified bundle

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the source tree is dirty on
`vibe` at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b` (unrelated changes were
preserved). I built the generated msvcrt AOT table as an opt-in test, then
restored the normal build. The restored artifact hashes are JS
`b5ccbe12c0672697bea8b5c8963aa7a0dcab841da23d9df16a18bc91ca92ff1b`, WASM
`104b9f10660c2456fa6c1587a6e2c89c2346acbf074ed2cbc0bb59381fed75e9`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index.html
`54d81676870b356aa800b9dd44bf8c27276f886edf8bb4bb8da80f85dafdf571`.

Observation: with `WASM_MSVCRT_JIT=1`, the opt-in bundle logged
`msvcrt JIT 21145 translated...` but died before the first frame with
`FATAL worker exception: memory access out of bounds`; it was not promoted.
The restored canonical smoke run reached `flips=758` at `t=17.0` (`682.2 FPS`),
then the known mapper interval (`flips=780`, `21.9 FPS`, followed by zero while
the worker remained alive), with canvas hash `e2251540` at 640x400 and no
`FATAL`, `RuntimeError`, or `JITBAD`.

Hypothesis: the generated msvcrt table has an address/ABI or block-boundary bug
and needs differential isolation before it can be used. The next performance
target remains runtime-generated game code in `0x0073xxxx` and `0x007axxxx`;
the verified GL inthash fast path stays enabled.

## 2026-08-31 23:39 IDT: fresh bundle clears reported black-screen repro

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the source tree is dirty on
`vibe` at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b` (unrelated changes were
preserved). The same freshly built bundle is served by ports 8799, 8806, and
8925. Hashes are JS
`b5ccbe12c0672697bea8b5c8963aa7a0dcab841da23d9df16a18bc91ca92ff1b`, WASM
`104b9f10660c2456fa6c1587a6e2c89c2346acbf074ed2cbc0bb59381fed75e9`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index.html
`54d81676870b356aa800b9dd44bf8c27276f886edf8bb4bb8da80f85dafdf571`.

Observation: clean browser checks at both the canonical URL and bare
`http://localhost:8799/` logged WebGL 2 context creation, the native swap and
mapper hooks, and a non-black canvas hash `e2251540` at 640x400. The page
reported its first frame at roughly 14–15 seconds; before that, compilation,
asset loading, and sound precache leave the canvas black. No `FATAL`,
`RuntimeError`, or `JITBAD` was emitted. The run then hit the known long render
interval around 20 seconds (`flips=3777`, then near-zero FPS) but the worker
did not die.

Hypothesis: the reported black screen is either the expected first-boot wait or
an old cached tab, not the current bundle's GL-table change. Reload the exact
canonical URL with a hard refresh and leave it open through the first-frame
wait; the page's “Starting Wine…” overlay should remain visible until the
first frame arrives. If it remains black after 30 seconds, the next useful
input is the page's status/log text and browser console error.

## 2026-08-31 21:44 IDT: qrhline SMC setup completed and promoted

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the source tree is dirty on
`vibe` at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`. Unrelated work was
preserved. The served artifact hashes are JS
`b5ccbe12c0672697bea8b5c8963aa7a0dcab841da23d9df16a18bc91ca92ff1b`, WASM
`ea1ed96804bb59e802d0f5334d6f02e64fe34730fe920963845bec048a8f8865`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index.html
`54d81676870b356aa800b9dd44bf8c27276f886edf8bb4bb8da80f85dafdf571`.

Observation: the setup prologue now passes its opcode skeleton check and logs
`native setupqrhlineasm4 @ 00633290` alongside
`native qrhlineasm4 @ 00633310`. It updates both SMC copies of each fixed-point
step and the palette/texture operands. The normal (non-experimental) browser
run reached `flips=740` at `t=16.6` (`617.2 FPS`) and `flips=1592` at `t=17.7`
(`818.5 FPS`), with no `FATAL`, `JITBAD`, or QRHLINE mismatch. The canvas was
640x400 and the RGBA presenter remained active.

Hypothesis: the previous black-screen behavior came from arming the qrhline
body without its SMC setup routine. The pair is now enabled by default; the
remaining long stalls later in the attract sequence are a separate mapper
hotspot (the profile points at vline/single-column work), not a qrhline setup
failure.

## 2026-08-31 21:02 IDT: black-screen regression isolated and shipped workaround

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the source tree is dirty on
`vibe` at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`. Unrelated work was
preserved. The served artifact hashes are JS
`b5ccbe12c0672697bea8b5c8963aa7a0dcab841da23d9df16a18bc91ca92ff1b`, WASM
`30b86cc05bf2a0dd20872537b185703f5313b142429069d1edc6794ac2648333`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index.html
`54d81676870b356aa800b9dd44bf8c27276f886edf8bb4bb8da80f85dafdf571`.

Observation: a canonical run installed the GL hooks, created the WebGL 2
context, and began presenting at `t=13.8` (`flips=84`, `fps=83.9`). The native
`qrhlineasm4` experiment then reproduced the reported failure: frames stopped
after a mapper pass and the page appeared black. With only
`WASM_NO_QRHLINE=1`, the same run sustained about 88 FPS through the test
window. The shipped build therefore no longer arms qrhline by default;
`WASM_EXPERIMENT_QRHLINE=1` is required for experiments. No `FATAL` or `JITBAD`
was emitted. The canvas is 640x400 and the RGBA presenter remains the default.

Hypothesis: the native qrhline implementation still lacks its complete
SMC-setup path and is unsafe for the normal browser build. The workaround
restores the interpreter implementation for that mapper while retaining the
other verified hooks; the qrhline setup/native implementation remains the next
focused optimization.

## 2026-08-31 20:37 IDT: verified current swap-thunk bundle under warm load

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the source tree is dirty on
`vibe` at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`. Unrelated work was
preserved. The served artifact hashes are JS
`b5ccbe12c0672697bea8b5c8963aa7a0dcab841da23d9df16a18bc91ca92ff1b`, WASM
`0ca6bf7756fa1f9bab153efb721c06fb3afde54cc290c64479b4d1ed505b7ab9`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index.html
`54d81676870b356aa800b9dd44bf8c27276f886edf8bb4bb8da80f85dafdf571`.

Observation: a clean run at
`http://localhost:8806/?WASM_TPUT=1` logged
`native wglSwapBuffers @ 3b84ebc0 (exact)` and then
`FPSSAMPLE t=18.9 flips=750 fps=405.9` followed by
`FPSSAMPLE t=19.9 flips=773 fps=23.0`; the latter is the known long mapper
stall, not a crash. No `FATAL` or `JITBAD` was emitted. The exact-address hook
was therefore retained and the browser artifact remained render-capable.

Hypothesis: the swap wrapper is no longer part of the interpreted hot page;
the next material FPS gain still requires reducing the periodic mapper/SSE
stall rather than adding more GL wrapper hooks.

## 2026-08-31 20:35 IDT: collision-safe wglSwapBuffers thunk

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the source tree is dirty on
`vibe` at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`. Unrelated work was
preserved. The current served artifact hashes are JS
`b5ccbe12c0672697bea8b5c8963aa7a0dcab841da23d9df16a18bc91ca92ff1b`, WASM
`0ca6bf7756fa1f9bab153efb721c06fb3afde54cc290c64479b4d1ed505b7ab9`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index.html
`54d81676870b356aa800b9dd44bf8c27276f886edf8bb4bb8da80f85dafdf571`.

Observation: the render-scoped profile identified `wglSwapBuffers` as the
largest remaining interpreted GL wrapper family (page `3b840000`, roughly
20–26% of post-frame interpreted entries). Its export is at runtime
`3b84ebc0`, but it collides with the compact native-hook hash table. The bundle
now uses an exact-address side check and a return-aware unix-call ABI
(`wgl_wglSwapBuffers`, code 8, `{ TEB, HDC, BOOL ret }`) without displacing
other hooks. The browser run at
`http://localhost:8806/?WASM_GLCOUNT=1&WASM_TPUT=1` logged
`native wglSwapBuffers @ 3b84ebc0 (exact)`, reached `flips=3209` at `t=28.6`,
and continued to `flips=3226`; no `FATAL` or `JITBAD` appeared. The canvas was
640×400 and received the corresponding frame stream; the run was stopped
during a long render stall, so its final page FPS was not used as a benchmark.

Hypothesis: this removes a substantial wrapper/interpreter share from the GL
path, but the observed scene is dominated by periodic mapper stalls, so a
stable FPS gain requires the next mapper/SSE optimization and a matched A/B
run.

## 2026-08-31 19:45 IDT: direct glTexSubImage2D thunk

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the source tree is dirty on
`vibe` at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`. Unrelated work was
preserved. The current served artifact hashes are JS
`b5ccbe12c0672697bea8b5c8963aa7a0dcab841da23d9df16a18bc91ca92ff1b`, WASM
`113c78e3e776aa3a76bb5d57ec73e6e21665b0530336c295574b07243f8ef51d`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index.html
`54d81676870b356aa800b9dd44bf8c27276f886edf8bb4bb8da80f85dafdf571`.

Observation: the hot interpreted range `3b821d50..3b821e07` disassembles to
Wine's exported `glTexSubImage2D` wrapper. The browser bundle now registers
that exact export as a generic GL thunk using unix-call index 316 and nine
unchanged stack arguments. A run at
`http://localhost:8806/?WASM_GLCOUNT=1&WASM_TPUT=1` logged
`native glTexSubImage2D @ 3b821d50`, then counted 1,262 calls in one active
sample and 723 presented frames; it had no `FATAL` or `JITBAD` output. The
canvas remained live and frames continued through the hook.

Hypothesis: this removes one interpreted Wine marshalling wrapper per texture
upload, but the measured call volume is only about 1.7 calls/frame in this
scene, so it is a small optimization rather than the main FPS lever. The next
profile should move past this wrapper and isolate the long `vlineasm1`/masked
mapper stalls.

## 2026-08-31 19:18 IDT: hardened default against accidental black GL path

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the source tree is dirty on
`vibe` at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`. Unrelated work was
preserved. The rebuilt served artifact hashes are JS
`b5ccbe12c0672697bea8b5c8963aa7a0dcab841da23d9df16a18bc91ca92ff1b`, WASM
`1bf7a5450b5e47acd8bcadeb0372eb8e3774daa5ac406ca66c646e30b061313c`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index.html
`54d81676870b356aa800b9dd44bf8c27276f886edf8bb4bb8da80f85dafdf571`.

Observation: the page now ignores the legacy
`WASM_ENABLE_GLPRESENT=1` query switch and selects `WASM_NO_GLPRESENT=1` unless
the new diagnostic-only `WASM_EXPERIMENTAL_GLPRESENT=1` switch is used. A
browser run using the old GL-enabled URL
`http://localhost:8806/?WASM_ENABLE_GLPRESENT=1&WASM_TPUT=1` reached
`OpenGL context: version 3.3`, then produced `FPSSAMPLE ... flips=729` and
later `flips=748`; the canvas therefore received real frames. The run was
stopped after the frame result was captured. No `FATAL` or `JITBAD` appeared.
The loading overlay now states that a cold first boot can take up to 40 seconds.

Hypothesis: the reported black screen was caused either by an old bookmarked
GL diagnostic URL or by mistaking the unusually long cold startup for a dead
canvas. The verified RGBA path remains the shipped path; the experimental GL
path is still intentionally available only for focused debugging.

## 2026-08-31 18:52 IDT: warm-profile attempt inconclusive

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the source tree is dirty on
`vibe` at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`. The current served
artifact hashes remain JS `b5ccbe12c0672697bea8b5c8963aa7a0dcab841da23d9df16a18bc91ca92ff1b`,
WASM `306bc51df92ab926a515e12326cb54a83ff3b6f1b221ad87d2db7acc9be19adb`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index.html
`f279f0aaece8930a0a080dadec9dfe7af5659eb2d2525f167e701dfb10ab2b8d`.

Observation: the clean run booted the supplied executable and reached the
verified native hooks, but its 26-second `WASM_STALLBT` observation was stopped
before a warm-frame result; the earlier clean run remains the authoritative
visible-frame result. No source or artifact change was made from this attempt.

Hypothesis: repeated long startup/mapper stalls make the current CDP profile
unsuitable for isolating `3b821d*`; a bounded guest-side counter is needed next.

## 2026-08-31 18:45 IDT: restored baseline after candidate rejection

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the persistent server serves
the rebuilt bundle from `/Users/alonamir/.webwine-work/web`. The source tree is
dirty on `vibe` at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`; unrelated work was
preserved. Artifact hashes are unchanged from the 18:15 build: JS
`b5ccbe12c0672697bea8b5c8963aa7a0dcab841da23d9df16a18bc91ca92ff1b`, WASM
`306bc51df92ab926a515e12326cb54a83ff3b6f1b221ad87d2db7acc9be19adb`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index.html
`f279f0aaece8930a0a080dadec9dfe7af5659eb2d2525f167e701dfb10ab2b8d`.

Observation: after reverting the unsafe `inthash` interior shortcut, the
baseline again produced a real `render: 320×200` frame with screenshot hash
`e2251540` and no `FATAL`, `JITBAD`, or `UNIMPLEMENTED`. A subsequent
frame-scoped profile did not reach gameplay within its observation window and
showed only startup `palswap` diagnostics; it is not used as an FPS result.

Hypothesis: the remaining `3b821d*` family still needs a call-level trace to
prove whether it contributes to warm frames; no optimization is promoted from
the current evidence.

## 2026-08-31 18:43 IDT: rejected unsafe inthash interior shortcut

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the persistent server serves
the rebuilt bundle from `/Users/alonamir/.webwine-work/web`. The source tree is
dirty on `vibe` at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`; unrelated work was
preserved. The restored served hashes are JS
`b5ccbe12c0672697bea8b5c8963aa7a0dcab841da23d9df16a18bc91ca92ff1b`, WASM
`306bc51df92ab926a515e12326cb54a83ff3b6f1b221ad87d2db7acc9be19adb`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index.html
`f279f0aaece8930a0a080dadec9dfe7af5659eb2d2525f167e701dfb10ab2b8d`.

Observation: a candidate that intercepted `inthash_find` interior AOT entries
removed the `0052a6*` misses but failed the browser gate: a 30-second run had
zero flips and repeated `palswap data too large for tilesheet!` errors. The
candidate was reverted and rebuilt. The restored run reached a real
`render: 320×200` frame with screenshot hash `e2251540`, and the unsafe
shortcut is absent from the current source.

Hypothesis: those interior entries do not share enough verified ABI state for a
generic return shortcut; the next optimization target must use a complete
function-level proof or remain interpreted.

## 2026-08-31 18:15 IDT: fixed shipped black canvas

The canonical URL is
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the persistent server serves
the rebuilt bundle from `/Users/alonamir/.webwine-work/web`. The source tree is
dirty on `vibe` at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`; unrelated work was
preserved. Served artifact hashes are JS
`b5ccbe12c0672697bea8b5c8963aa7a0dcab841da23d9df16a18bc91ca92ff1b`, WASM
`306bc51df92ab926a515e12326cb54a83ff3b6f1b221ad87d2db7acc9be19adb`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index.html
`f279f0aaece8930a0a080dadec9dfe7af5659eb2d2525f167e701dfb10ab2b8d`.

Observation: the default browser path now forces the verified RGBA frame
presenter. A clean run reached `render: 320×200`, `frames: 5537`, and a
non-black screenshot (`shotBytes=7221`, hash `e2251540`) at 13 seconds, with
no `FATAL` or `JITBAD`; OpenGL context creation still completed at
`GFX| OpenGL context: version 3.3`. The previously shipped GL transfer path
produced an all-zero framebuffer in the same run. Bitmap transfer remains
available explicitly with `WASM_ENABLE_GLPRESENT=1`; browsers without
`transferToImageBitmap()` now have an RGBA readback fallback.

Hypothesis: this restores visible gameplay across the current browser/GPU
combination while the GL default-framebuffer issue is isolated separately.

## 2026-08-31 17:57 IDT: eliminated empty SDL poll fallback

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the persistent server serves
the rebuilt bundle from `/Users/alonamir/.webwine-work/web`. The source tree is
dirty on `vibe` at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`; unrelated work was
preserved. Served artifact hashes are JS
`f94a198753bcfbb5883ac7898c2c2a1f25cfd50ae05862568757793de76c3359`, WASM
`6217684b1309ee41799365fbe1926ed63a7c39175a0171381e59dee27e72fa02`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
`index.html`
`d811828f4add4d00be577f85a8d20fe4e87a66351948a1b5c7005ddb6310545a`.

Observation: empty `SDL_PollEvent` calls now return natively when no browser
input is pending; keyboard, absolute-mouse, and button ring events set a bounded
fallback window for the real SDL queue, while relative motion/buttons retain
their native synthesis. The active profile no longer lists `006a8420`. A clean
run with `Enter@20` continued rendering, reached warm guest samples around
`790–940 FPS` in the screenshot harness, and had no `FATAL`, `JITBAD`,
`JITBADEIP`, or `UNIMPLEMENTED`.

Hypothesis: this removes a high-frequency dispatch-only cost without affecting
input delivery; the next profile target is the remaining `0052a6aa`/Wine thunk
family and low-frequency GL wrapper code.

## 2026-08-31 17:49 IDT: native vlineasm1 entry/prologue

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the persistent server serves
the rebuilt bundle from `/Users/alonamir/.webwine-work/web`. The source tree is
dirty on `vibe` at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`; unrelated work was
preserved. Served artifact hashes are JS
`f94a198753bcfbb5883ac7898c2c2a1f25cfd50ae05862568757793de76c3359`, WASM
`5976e67bfc0f1752b42b0327036abbd9499aa6655217019b53a7890f694b4561`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
`index.html`
`d811828f4add4d00be577f85a8d20fe4e87a66351948a1b5c7005ddb6310545a`.

Observation: the runtime-traced `00631c90` cdecl setup was folded into the
existing native `vlineasm1` loop, including its five saved registers, six
argument loads, zero-count return, `EBP` frame, and count increment. The hook
armed as `native vlineasm1 entry @ 00631c90`; the active profile no longer lists
any `00631c90–00631cf1` entries. A clean 30-second run reached warm samples of
`~800–900 FPS` under the screenshot harness and showed stable mapper counters
(`v1=2/400`, `mv1=70/1,180`) with no `FATAL`, `JITBAD`, `JITBADEIP`, or
`UNIMPLEMENTED`.

Hypothesis: this removes the remaining single-column setup overhead, though
the screenshot harness adds substantial readback stalls, so the next A/B should
use the same clean URL and compare warm guest samples rather than harness FPS.

## 2026-08-31 17:40 IDT: isolated remaining active-frame residue

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_IPAGE=1&WASM_IPAGE_FRAME=1`; the
persistent server serves the rebuilt bundle from
`/Users/alonamir/.webwine-work/web`. The source tree is dirty on `vibe` at
`ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`; unrelated work was preserved.
Served artifact hashes are JS
`f94a198753bcfbb5883ac7898c2c2a1f25cfd50ae05862568757793de76c3359`, WASM
`1e3f26a5a2389d2a5bf5fe59d601e2364471315f75d7c3fd539ddfc71d52dc5c`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
`index.html`
`d811828f4add4d00be577f85a8d20fe4e87a66351948a1b5c7005ddb6310545a`.

Observation: the active-frame profile removed `006f33a0/1` and
`004da630/32/39` from the hot miss set. The remaining material executable
residue is `00631c90–00631cf1`, the self-modifying single-column mapper setup;
its already-native loop calls were `v1=2/406` and `mv1=71/1194` in a warm
sample. Warm samples reached `999–1001 FPS` after transition, with no `FATAL`,
`JITBAD`, `JITBADEIP`, or `UNIMPLEMENTED`.

Hypothesis: removing the mapper prologue/setup would be the next meaningful
FPS lever, but its runtime stack shape is not proven by the current skeleton.
The full native hook needs a captured entry/return ABI and differential run;
the current bundle is retained unchanged until that evidence exists.

## 2026-08-31 17:26 IDT: rejected unsafe inthash interior shortcut

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_IPAGE=1&WASM_IPAGE_FRAME=1`; the
persistent server serves the rebuilt bundle from
`/Users/alonamir/.webwine-work/web`. The source tree is dirty on `vibe` at
`ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`; unrelated work was preserved.
Served artifact hashes are JS
`f94a198753bcfbb5883ac7898c2c2a1f25cfd50ae05862568757793de76c3359`, WASM
`1e3f26a5a2389d2a5bf5fe59d601e2364471315f75d7c3fd539ddfc71d52dc5c`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
`index.html`
`d811828f4add4d00be577f85a8d20fe4e87a66351948a1b5c7005ddb6310545a`.

Observation: a candidate fast path for `0052a6aa` did not match the runtime
register/frame state; the active profile continued to show
`0052a6aa/af/a7` and the neighboring `3b821d…` thunk family. It was removed
and the known-good feature-predicate bundle was rebuilt. The run remained free
of `FATAL`, `JITBAD`, `JITBADEIP`, and `UNIMPLEMENTED`; warm guest samples were
around `999–1001 FPS` after transition.

Decision: do not optimize the generic `inthash_find` interior without tracing
its actual ABI. The next target remains the low-frequency GL/thunk family or a
broader SSE/x87 translated region.

## 2026-08-31 17:14 IDT: removed two hot runtime wrapper misses

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the persistent server serves
the rebuilt bundle from `/Users/alonamir/.webwine-work/web`. The source tree is
dirty on `vibe` at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`; unrelated work was
preserved. Served artifact hashes are JS
`f94a198753bcfbb5883ac7898c2c2a1f25cfd50ae05862568757793de76c3359`, WASM
`1e3f26a5a2389d2a5bf5fe59d601e2364471315f75d7c3fd539ddfc71d52dc5c`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
`index.html`
`d811828f4add4d00be577f85a8d20fe4e87a66351948a1b5c7005ddb6310545a`.

Observation: exact miss-only fast paths were added for the runtime-generated
`006f33a0` wrapper (preserving `push ebx`, stack subtraction, `0x8000` argument,
and entry into translated `0073ac20`) and the `004da630` feature predicate
(including conditional flags, bounded pointer read, and cdecl return). The
active profile removed `006f33a0/1` and `004da630/32/39` from `IMISS top`.
A clean run logged `JIT 170732 translated blocks loaded`, had no `FATAL`,
`JITBAD`, `JITBADEIP`, or `UNIMPLEMENTED`, and reached sustained guest samples
around `999–1001 FPS` after the attract-loop transition, with `jit_frac=98.1–98.2%`.

Hypothesis: the fixed presentation ceiling now dominates the warm attract-loop
samples; the next measurable residue is `0052a6aa`/GL-state dispatch and the
remaining Wine thunk family. No correctness regression was observed.

## 2026-08-31 16:55 IDT: fixed false black-screen loading state

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the persistent server serves
the rebuilt bundle from `/Users/alonamir/.webwine-work/web`. The source tree is
dirty on `vibe` at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`; unrelated work was
preserved. Served artifact hashes are JS
`f94a198753bcfbb5883ac7898c2c2a1f25cfd50ae05862568757793de76c3359`, WASM
`b13af80d3f5bfac83964c4da2c2cea81cfcc19b6cdbfae01a3a4e071e4675bee`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
`index.html`
`d811828f4add4d00be577f85a8d20fe4e87a66351948a1b5c7005ddb6310545a`.

Observation: the browser run received frames at 640x400, logged
`JIT 170732 translated blocks loaded`, reached about 1,700 FPS during startup,
and had no `FATAL`, `JITBAD`, `JITBADEIP`, or `UNIMPLEMENTED`. The captured frame
at 11 seconds visibly contained the 3D Realms intro image. An earlier capture
was genuinely dark while the intro transitioned, but the old loading overlay
remained on top because it required sampled brightness >= 36.

Change: the overlay now hides on the first received frame, including a valid
dark frame; a worker failure still replaces it with the explicit stopped-worker
message. This removes the false black-screen presentation without changing the
Wine or WebGL execution path.

Hypothesis: any remaining several-second frame gap during the intro is the
existing expensive startup/render transition, not a dead browser worker. The
next test should hard-refresh the canonical URL and allow the initial load to
finish before judging the screen.

## 2026-08-31 16:42 IDT: optimized adjacent runtime wrapper

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the persistent server serves
the rebuilt bundle from `/Users/alonamir/.webwine-work/web`. The source tree is
dirty on `vibe` at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`; unrelated work was
preserved. Served artifact hashes are JS
`f94a198753bcfbb5883ac7898c2c2a1f25cfd50ae05862568757793de76c3359`, WASM
`c3e7faa31e729e21901df9b9dbcd184167ebfa5b5695ec4f0c7974e7b4b40d3c`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
`index.html`
`47989fc6427db4b652d670d7602328c426d535ba10035c717bed1027a7365412`.

Observation: the adjacent runtime-generated `008066f0…0080673c` wrapper was
decoded and optimized, including its `push ebx`, `sub esp,0x38`, argument
shuffle, direct call, shared stores, `add esp,0x38` flags, saved-register pop,
and return. The active profile no longer lists that wrapper family in `IMISS
top`; the remaining top entries are Wine thunk padding (`3f79…`), `006f33a0/1`,
and `004da630…`. The clean correctness run reached changing 640x400 output,
logged `JIT 170732 translated blocks loaded`, and produced repeated warm
samples around 1,000 FPS. No `FATAL`, `JITBAD`, `JITBADEIP`, or `UNIMPLEMENTED`
occurred; input was ready.

Decision: retain this candidate. Continue from the newly exposed Wine thunk/
runtime family rather than revisiting the removed `0080…` wrappers.

## 2026-08-31 16:34 IDT: optimized runtime compiled-code wrapper

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the persistent server serves
the rebuilt bundle from `/Users/alonamir/.webwine-work/web`. The source tree is
dirty on `vibe` at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`; unrelated work was
preserved. Served artifact hashes are JS
`f94a198753bcfbb5883ac7898c2c2a1f25cfd50ae05862568757793de76c3359`, WASM
`89d9821b16b633b8b7f35ca708ca80280838dd110c51b4a703902b20b19614d9`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
`index.html`
`47989fc6427db4b652d670d7602328c426d535ba10035c717bed1027a7365412`.

Observation: the runtime-generated `00806700…0080673c` argument-shuffle/direct-
call wrapper was optimized as one exact byte-pattern path, including its
post-call stores, stack unwind, saved `EBX`, and return. The active profile
removed the whole wrapper family from `IMISS top`. A clean run logged
`JIT 170732 translated blocks loaded`, reached changing 640x400 output, and
reported repeated warm samples of `999.3–1000.3 FPS` with `jit_frac=98.1–98.2%`
and `kinsn/frame` around 193–197. No `FATAL`, `JITBAD`, `JITBADEIP`, or
`UNIMPLEMENTED` occurred; input was ready.

Hypothesis: this is a real reduction in interpreted render residue, while the
fixed attract-loop FPS is now close to the harness's presentation/scene limit;
the next gain must come from the remaining `008066f0…` wrapper or the native
single-column mapper rather than this already-eliminated family.

## 2026-08-31 16:21 IDT: promoted explicit executable AOT roots

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the persistent server serves
the rebuilt bundle from `/Users/alonamir/.webwine-work/web`. The source tree is
dirty on `vibe` at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`; unrelated work was
preserved. Served artifact hashes are JS
`f94a198753bcfbb5883ac7898c2c2a1f25cfd50ae05862568757793de76c3359`, WASM
`a4d9500fe06b241d9860dc7646ab548bb2cb289618f43b0fc9f47a5226e5953d`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
`index.html`
`47989fc6427db4b652d670d7602328c426d535ba10035c717bed1027a7365412`.

Observation: seven profiler-confirmed executable roots were added to the AOT
generator (`004e73b0`, `00506c30`, `00572cd0`, `0057aa20`, `006b12f0`,
`006b1650`, `006f33a4`). The table grew from `170725/170748` to
`170732/170755` blocks. An active-frame profile showed those entries no longer
in `IMISS top`; the remaining top entries are runtime-generated `0080…`, Wine
thunk padding, and an internal `006f33a1` target. A clean 30-second run logged
`JIT 170732 translated blocks loaded`, reached changing 640x400 output, and
showed warm samples up to `999.9 FPS` with `jit_frac=98.4%`, no `FATAL`,
`JITBAD`, `JITBADEIP`, or `UNIMPLEMENTED`.

Decision: retain the explicit-root AOT candidate as the current shipped build.
The measured miss reduction is real; the exact FPS delta remains workload- and
transition-sensitive, so no fixed percentage claim is made. The next measured
target is the runtime-generated `0080…` callback/compiled-code family.

## 2026-08-31 15:58 IDT: rejected callback-veneer fast path

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the persistent server serves
the rebuilt bundle from `/Users/alonamir/.webwine-work/web`. The source tree is
dirty on `vibe` at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`; unrelated work was
preserved. The restored served artifact hashes are JS
`f94a198753bcfbb5883ac7898c2c2a1f25cfd50ae05862568757793de76c3359`, WASM
`a7af71127b6cbe0d1b65135415ea28c292a0e155db71f52682eb5fe6b227e7f6`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
`index.html`
`47989fc6427db4b652d670d7602328c426d535ba10035c717bed1027a7365412`.

Observation: the measured callback veneer pattern
`mov eax,index; mov edx,veneer; call edx; ret 0x14` was tested with exact
return-address preservation and the veneer’s small dispatch index. It failed
immediately with `FATAL worker exception: memory access out of bounds`, zero
frames, and a black canvas. The candidate was removed. A subsequent recovery
run logged `JIT 170725 translated blocks loaded`, reached changing 640x400
output (`first-frame: 9.0s`), and had no `JITBAD`, `JITBADEIP`, `UNIMPLEMENTED`,
or worker/runtime failure.

Decision: do not bypass the callback trampoline at the interpreter boundary.
The neutral thunk-prefix micro-optimization was also removed; continue with
the SSE/x87 block-translation path.

## 2026-08-31 15:34 IDT: fixed black-screen appearance during startup

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the persistent server serves
the rebuilt bundle from `/Users/alonamir/.webwine-work/web`. The source tree is
dirty on `vibe` at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`; unrelated work was
preserved. Served artifact hashes are JS
`f94a198753bcfbb5883ac7898c2c2a1f25cfd50ae05862568757793de76c3359`, WASM
`a7af71127b6cbe0d1b65135415ea28c292a0e155db71f52682eb5fe6b227e7f6`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
`index.html`
`47989fc6427db4b652d670d7602328c426d535ba10035c717bed1027a7365412`.

Observation: the page's visible-pixel test was followed by an unconditional
`bootEl.hidden = true` in both frame handlers. A black startup/transition frame
therefore hid the loading notice and looked like a dead black screen. Those two
unconditional hides were removed. A fresh 22-second CDP run logged
`JIT 170725 translated blocks loaded`, showed the loading notice over black
canvas at 3s and 8s, showed a visible 3D Realms frame at 15s, and showed the
Duke Nukem 3D title at 21s. It reached changing 640x400 output, with no
`FATAL`, `RuntimeError`, `JITBAD`, or `JITBADEIP`; input was ready.

Hypothesis: this resolves the reported black screen when it was caused by the
normal startup/transition period. If a browser still remains black after the
loading notice changes to `Wine worker stopped`, the remaining cause is a
worker/runtime failure rather than the overlay race.

## 2026-08-31 14:36 IDT: restored stable dispatch after thunk A/B

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; ports 8799 and 8925 serve
the same current bundle. The dirty `vibe` tree remains at
`ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`, with unrelated changes
preserved. The retained served artifacts are JS
`f94a198753bcfbb5883ac7898c2c2a1f25cfd50ae05862568757793de76c3359`, WASM
`327672bd730f061c341335717104ef7b870d9ac1049095232d92f8612d667bfb`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
`index.html`
`c1207668170412c29fa31e35ff95f543c59ffdf35fb7db940c65a67c5a240f1b`.

Observation: retrying the runtime-generated absolute-jump shortcut with the
missing `pop ebp` state corrected still reproduced
`UNIMPLEMENTED opcode e0 at eip=3f9b5561` and zero frames. Removing the
shortcut and rebuilding restored the regular path: the recovery run logged
`JIT 170725 translated blocks`, reached changing 640x400 output with
`first-frame: 10.7s`, and its later samples reached approximately 777–910 FPS
while the attract sequence was still settling. No `FATAL`, `RuntimeError`,
`JITBAD`, or `JITBADEIP` appeared; the canvas was non-black and input was
ready. The delayed diagnostic dump captured the thunk bytes, but the target
transfer is not safe to bypass at this layer.

Decision: the generic dynamic-thunk optimization is rejected and is absent
from the retained bundle. The memset tail aliases remain correctness-clean
but measured neutral; the next performance work must preserve the runtime
trampoline protocol or target SSE/x87 and the native single-column mapper.

## 2026-08-31 14:34 IDT: rejected generic runtime-thunk jump shortcut

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; ports 8799 and 8925 serve
the same current bundle. The dirty `vibe` tree remains at
`ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`, with unrelated changes
preserved. The retained served artifacts are JS
`f94a198753bcfbb5883ac7898c2c2a1f25cfd50ae05862568757793de76c3359`, WASM
`327672bd730f061c341335717104ef7b870d9ac1049095232d92f8612d667bfb`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
`index.html`
`c1207668170412c29fa31e35ff95f543c59ffdf35fb7db940c65a67c5a240f1b`.

Observation: the guarded MSVCRT memset internal-entry candidate remained
correct but neutral in the warm window (approximately 998–1000 FPS), so no
performance claim was made. A subsequent generic fast path for runtime
`0x03…` absolute-indirect jump thunks was tested and immediately rejected:
the exact URL logged `UNIMPLEMENTED opcode e0 at eip=3f9b5561` and produced
zero frames. After removing that path and rebuilding, a fresh run logged
`JIT 170725 translated blocks`, reached changing 640x400 output after the
variable startup/precaching interval, and reached approximately 994–1,000 FPS
in the final warm samples with no `FATAL`, `RuntimeError`, `JITBAD`, or
`JITBADEIP`. The canvas was non-black and input remained ready.

Decision: keep the stable bundle and discard the generic thunk shortcut. The
captured thunk bytes are retained as evidence, but their target cannot safely
be substituted by an interpreter-level EIP transfer in this runtime. Continue
with a narrower runtime-generated-code strategy or the SSE/x87 path.

## 2026-08-31 14:05 IDT: made the boot state visible

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; ports 8799 and 8925 serve
the same current bundle. The dirty `vibe` tree remains at
`ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`, with unrelated changes
preserved. The served artifacts are JS
`f94a198753bcfbb5883ac7898c2c2a1f25cfd50ae05862568757793de76c3359`, WASM
`ebce8f5a665e3582c3b4985a025289b916e75ad207ad85c14c30ee44aac72f41`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
`index.html`
`c1207668170412c29fa31e35ff95f543c59ffdf35fb7db940c65a67c5a240f1b`.

Observation: the page now shows `Starting Wine… Loading the game (usually
5–10 seconds)` over the black canvas until the first frame, hides it when a
real frame arrives, and reports `Wine worker stopped. Reload this page to
retry.` if the worker dies. A 10-second CDP run at the exact URL loaded
`JIT 170725 translated blocks`, reached the first frame at about 6.7s, and
reported changing 640x400 output with no `FATAL`, `RuntimeError`, `JITBAD`, or
`JITBADEIP`. The 3s screenshot contains the boot overlay; the later frame is
non-black gameplay/title output. The only browser error was an unrelated 404
resource request.

Hypothesis: a user seeing black indefinitely is likely using a stale tab/cache
or a browser/runtime that cannot start the worker; this page now distinguishes
the normal startup interval from that failure. A permanent black result still
needs the page's visible worker message and browser console to identify the
remaining environment-specific cause.

## 2026-08-31 13:56 IDT: AOT-translated fixed single-column mapper setup

The canonical URL is
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; ports 8799 and 8925 serve
the same current bundle. The dirty `vibe` tree remains at
`ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`, with unrelated changes
preserved. The served artifacts are JS
`f94a198753bcfbb5883ac7898c2c2a1f25cfd50ae05862568757793de76c3359`, WASM
`ebce8f5a665e3582c3b4985a025289b916e75ad207ad85c14c30ee44aac72f41`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
`index.html` `70cd34d6459f3db983bfa1790b4dba6085accb6f5549ea7d377c5824179dccff`.

Observation: the generated table now translates `170725/170748` blocks and
explicitly includes the fixed setup leaders `0x00631c40`, `0x00631c60`, and
`0x00631c7d` while leaving the self-modifying loop window excluded. The
diagnostic run logged `JIT 170725 translated blocks loaded`; the former
`0x00631c69/0x00631c7d` setup-miss cluster no longer appears in `IMISS top`.
It reached a changing 640x400 canvas (`first-frame: 7.2s`, warm samples near
998 FPS), input ready, and audio active, with no `FATAL`, `RuntimeError`,
`JITBAD`, or `JITBADEIP`. The profile run itself reported `kinsn/frame` near
200 after warm-up; no standalone FPS percentage is claimed because the
diagnostic instrumentation changes load.

Decision: retain this correctness-checked AOT setup translation as the
current candidate and continue profiling the remaining CRT/engine residue.

## 2026-08-31 13:44 IDT: rejected focused MSVCRT AOT candidate

Built and tested the focused MSVCRT AOT candidate at
`http://localhost:8925/?WASM_TPUT=1&WASM_BADIP=1`. The source tree is dirty
and remains on `vibe` at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`; all
unrelated changes were preserved. Candidate hashes were JS
`f94a198753bcfbb5883ac7898c2c2a1f25cfd50ae05862568757793de76c3359`, WASM
`edacd807944969a937b3721af0fa7480d36136cb63a32aab22ae31152f0fe275`, and
data `d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: the candidate logged `msvcrt JIT 143 translated blocks loaded`
and created the WebGL 3.3 context, but hit a long intro stall (`flips=1112`
from roughly 18–24s) and reached only 230 frames at 25s. It later recovered,
but warm samples were approximately 700–850 FPS versus the regular bundle's
approximately 1,000 FPS under the same harness. The canvas was non-black and
input/audio were active, so this is a performance regression rather than a
boot failure.

Decision: rejected the focused MSVCRT AOT candidate and restored the verified
canonical artifacts: JS
`f94a198753bcfbb5883ac7898c2c2a1f25cfd50ae05862568757793de76c3359`, WASM
`b8df209aef45208e02c467cc033183baf2767ff98c2fba5192f21bb9b2fc6e5d`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, and
worker `72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`.
The next optimization should return to a sustained engine hotspot, not broad
MSVCRT block translation.

## 2026-08-31 13:34 IDT: fixed stale WASM/data payload pairing in browser worker

The canonical URL is
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; port 8799 serves the same
bundle. The dirty `vibe` tree remains at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`;
the source rebuild was attempted but was killed during the large
`wasm_x86.c` compile, so the verified WASM artifact was not replaced. The
served artifacts are JS
`f94a198753bcfbb5883ac7898c2c2a1f25cfd50ae05862568757793de76c3359`, WASM
`b8df209aef45208e02c467cc033183baf2767ff98c2fba5192f21bb9b2fc6e5d`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
`index.html` `70cd34d6459f3db983bfa1790b4dba6085accb6f5549ea7d377c5824179dccff`.

Observation: `webwine/browser/worker.js` now assigns Emscripten
`Module.locateFile` before importing the bundle, adding one boot token to both
the `.wasm` and `.data` requests as well as the JS request. The served worker
was updated without replacing the existing WASM. A fresh CDP run logged
`wasm_x86: JIT 170722 translated blocks loaded`, WebGL 3.3 context creation,
and changing 640x400 frames (`first-frame: 12.8s`, 1,023 FPS warm sample); no
`FATAL`, `RuntimeError`, `JITBAD`, or `JITBADEIP` occurred. The canvas was
non-black. Startup and intro transitions can remain black for roughly 10–13s
under load, but the worker now reports progress and cannot mix stale payloads.

Decision: use the exact URL above and hard-refresh once; retain the verified
WASM artifact and the cache-pairing fix.

## 2026-08-31 13:26 IDT: promoted AOT entries for compiled-code wrappers

The canonical URL is
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the same complete bundle
is served on port 8799. The dirty `vibe` tree remains at
`ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`, with unrelated dirty and
untracked work preserved. Final served artifacts are JS
`f94a198753bcfbb5883ac7898c2c2a1f25cfd50ae05862568757793de76c3359`, WASM
`b8df209aef45208e02c467cc033183baf2767ff98c2fba5192f21bb9b2fc6e5d`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, and
`index.html` `70cd34d6459f3db983bfa1790b4dba6085accb6f5549ea7d377c5824179dccff`.

Observation: the generated table is now `170722/170745` translated blocks
and includes explicit entries `0x006b0c80` and `0x007007b0`, alongside the
previous entries. In the diagnostic run, the prior `007007b0` and
`006b0c80/006b0c85` miss cluster disappeared from `IMISS top`; the remaining
top entries are MSVCRT `3ee39bxx`/`3ee3b5xx` startup and locale paths. The run
reported `JIT 170722 translated blocks loaded`, 11 non-black samples and 6
distinct frames, changing 640x400 output, input/audio active, and no
`FATAL`, `RuntimeError`, `JITBAD`, or `JITBADEIP`.

Decision: promote the two wrapper entries and continue profiling the newly
exposed MSVCRT/interpreter hotspot.

## 2026-08-31 12:56 IDT: retained corrected acquire-only bundle

The canonical URL is
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the same complete bundle
is also served on port 8799. The dirty `vibe` tree remains at
`ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`, with unrelated dirty and
untracked work preserved. Final served artifacts are JS
`f94a198753bcfbb5883ac7898c2c2a1f25cfd50ae05862568757793de76c3359`, WASM
`5501ed7011619ade082d49fe898c4f510d21a3a214573c838f09649e87ab13a3`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, and
`index.html` `70cd34d6459f3db983bfa1790b4dba6085accb6f5549ea7d377c5824179dccff`.

Observation: a final canonical CDP run completed against the full nonzero
WASM artifact, armed `native ntdll RtlAcquireSRWLockExclusive fast path @
3f928480`, reached a changing 640x400 frame at `first-frame: 18.2s`, and
reported input/audio active. No `FATAL`, `RuntimeError`, `JITBAD`, or
`JITBADEIP` appeared. The release-only A/B was clean on both sides but
neutral (`12,325` vs `12,419` warm-window flips), so the release hook was
removed; the acquire correction remains for correctness.

Decision: serve the corrected acquire-only artifact and continue seeking a
measurable optimization in the remaining waiter/wake or interpreted regions.

## 2026-08-31 12:47 IDT: corrected SRW ownership state and enabled release fast path

The candidate was tested at
`http://localhost:8925/?WASM_TPUT=1&WASM_BADIP=1`; the canonical served
endpoints remain 8799 and 8806. The dirty `vibe` tree remains at
`ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`, with unrelated dirty and
untracked work preserved. The served artifacts are JS
`f94a198753bcfbb5883ac7898c2c2a1f25cfd50ae05862568757793de76c3359`, WASM
`ea5ba1a5ae8071212c761709df3c12264ee0f0e319c0fc9b91e479ea891f0062`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, and
`index.html` `70cd34d6459f3db983bfa1790b4dba6085accb6f5549ea7d377c5824179dccff`.

Observation: the prior exclusive-acquire hook wrote only SRW bit 0, while
Wine stores `owners` in the high half-word; it could leave a lock appearing
unowned. The hook now requires state zero and atomically writes
`0x00010001` (one owner plus exclusive bit). The matching release hook is
guarded to atomically change only `0x00010001` to zero, leaving waiter states
to Wine’s wake protocol. The runtime logged both
`native ntdll RtlAcquireSRWLockExclusive fast path @ 3f928480` and
`native ntdll RtlReleaseSRWLockExclusive uncontended fast path @ 3f944060`.
The run reached changing 640x400 frames, input/audio active, and steady
`FPSSAMPLE` rates of approximately 0.98–1.00K FPS, with no `FATAL`,
`RuntimeError`, `JITBAD`, or `JITBADEIP`.

Decision: retain this correctness-critical candidate and use a release-only
control before claiming a throughput percentage; the larger waiter/wake path
remains the next optimization target.

## 2026-08-31 12:38 IDT: rejected native-dispatch jump-table experiment

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the dirty `vibe` tree is
still at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`, with unrelated dirty and
untracked work preserved. The served control artifact is JS
`f94a198753bcfbb5883ac7898c2c2a1f25cfd50ae05862568757793de76c3359`, WASM
`6989188a3e74a490cfe43910c07a2304db57296329420a78ae03b290087e49b3`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, and
`index.html` `70cd34d6459f3db983bfa1790b4dba6085accb6f5549ea7d377c5824179dccff`.

Observation: the post-AOT diagnostic URL
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1&WASM_IPAGE=1&WASM_MODULES=1`
shows that the `0070d7a0` cluster is gone; the remaining dominant miss cluster
is NTDLL `3f9440xx`, beginning at `RtlReleaseSRWLockExclusive @ 3f944060`.
The attempted front-loaded switch dispatch for native mapper calls produced
clean changing 640x400 frames, input/audio, and no `FATAL`, `RuntimeError`,
`JITBAD`, or `JITBADEIP`, but the enabled run and control both settled near
1,000 FPS in their comparable warm samples. The candidate WASM was
`f192966b788c4087568e765564cbee013c0fe3b6c408b045959a40a1bd02f34b`; it was
not promoted.

Decision: retain the control bundle and move on to the full SRW release/wake
path, where a partial ownership-only shortcut is insufficient.

## 2026-08-31 12:24 IDT: restored the previously advertised port 8799

The earlier test link used `http://localhost:8799/`, but that port was no
longer serving the rebuilt browser output; the current bundle was only
available on the canonical port 8806. Port 8799 is now served from the same
output directory as 8806. The dirty `vibe` tree remains at
`ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`; unrelated dirty and untracked
work is preserved. Both ports serve JS
`f94a198753bcfbb5883ac7898c2c2a1f25cfd50ae05862568757793de76c3359`, WASM
`6989188a3e74a490cfe43910c07a2304db57296329420a78ae03b290087e49b3`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, and
`index.html` `70cd34d6459f3db983bfa1790b4dba6085accb6f5549ea7d377c5824179dccff`.

Observation: a clean CDP run at the exact previously advertised URL
`http://localhost:8799/?WASM_TPUT=1&WASM_BADIP=1` logged
`JIT 170728 translated blocks loaded`, showed the first frame after startup,
and reached changing 640x400 frames with input/audio active. The captured
8-second image is still black during initialization; the 18-second image is
the rendered title transition, and the guest resumes at about 20 seconds.
There were no `FATAL`, `RuntimeError`, `JITBAD`, or `JITBADEIP` errors.

Decision: keep both 8799 and the documented canonical 8806 endpoint pointed
at the same verified bundle; reload the page so the browser drops any stale
worker/cache state.

## 2026-08-31 12:25 IDT: promoted explicit AOT entry for SDL_GetVideoDevice

The canonical URL is
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the dirty `vibe` tree is
still at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`, with unrelated dirty and
untracked work preserved. Served artifacts are JS
`f94a198753bcfbb5883ac7898c2c2a1f25cfd50ae05862568757793de76c3359`, WASM
`6989188a3e74a490cfe43910c07a2304db57296329420a78ae03b290087e49b3`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, and
`index.html` `70cd34d6459f3db983bfa1790b4dba6085accb6f5549ea7d377c5824179dccff`.

Observation: the generated AOT table now includes the exact
`SDL_GetVideoDevice` wrapper at `0x0070d7a0` (`170728/170751` translated
blocks), while the same-table control has `170727/170750`. The candidate’s
warm run reached `FPSSAMPLE t=28.7 flips=17941`; the control reached
`t=28.1 flips=14715`, a directionally favorable roughly 20% difference across
these noisy runs. Both rendered changing 640x400 frames, accepted input/audio,
and had no `FATAL`, `RuntimeError`, `JITBAD`, or `JITBADEIP`; the canonical
regression reached `t=21.2 flips=13306` with the candidate.

Decision: promote the explicit AOT entry and continue profiling the remaining
NTDLL release/dynamic executable misses.

## 2026-08-31 12:09 IDT: rejected SDL/video and exclusive-release micro-hooks

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the dirty `vibe` tree is
still at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`, with unrelated dirty and
untracked work preserved. The final served artifacts remain JS
`f94a198753bcfbb5883ac7898c2c2a1f25cfd50ae05862568757793de76c3359`, WASM
`dbdbe10bb5da42c560ecf1616b802eacb165274ed251fb0679a59a8e5223f239`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, and
`index.html` `70cd34d6459f3db983bfa1790b4dba6085accb6f5549ea7d377c5824179dccff`.

Observation: the SDL_GetVideoDevice wrapper and exact uncontended
RtlReleaseSRWLockExclusive CAS candidates both armed and rendered cleanly,
but neither produced a repeatable warm-run gain against its kill-switch
control. Both were removed; the canonical regression now arms only the
promoted empty-bucket RtlWakeAddressAll path at `3f949570`, reached
`FPSSAMPLE t=18.7 flips=7890`, and had changing frames, input, and no
`FATAL`, `RuntimeError`, `JITBAD`, or `JITBADEIP`.

Decision: retain the wake-all-only promotion and move the next effort to the
remaining dynamic executable/NTDLL region rather than accumulating neutral
micro-hooks.

## 2026-08-31 11:41 IDT: retained wake-all only after single-wake A/B

The canonical URL is
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the dirty `vibe` tree is
still at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`, with unrelated dirty and
untracked work preserved. Served artifacts are JS
`f94a198753bcfbb5883ac7898c2c2a1f25cfd50ae05862568757793de76c3359`, WASM
`dbdbe10bb5da42c560ecf1616b802eacb165274ed251fb0679a59a8e5223f239`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, and
`index.html` `70cd34d6459f3db983bfa1790b4dba6085accb6f5549ea7d377c5824179dccff`.

Observation: the exact canonical run armed only
`RtlWakeAddressAll empty-bucket fast path @ 3f949570`, reached
`FPSSAMPLE t=19.1 flips=6930`, and rendered changing 640x400 frames with
input ready. No `FATAL`, `RuntimeError`, `JITBAD`, or `JITBADEIP` occurred.
The separate `RtlWakeAddressSingle` experiment was removed after its matched
warm comparison ended at 10,301 flips versus 10,810 with
`WASM_NO_NTDLL_WAKE_FAST=1`; it is not promoted.

Decision: keep the wake-all promotion and continue with the remaining
exclusive-release/non-empty-wake cluster.

## 2026-08-31 11:31 IDT: promoted guarded empty-wake fast path

The canonical URL is
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the dirty `vibe` tree is
still at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`, with unrelated dirty and
untracked work preserved. The persistent canonical server now serves JS
`f94a198753bcfbb5883ac7898c2c2a1f25cfd50ae05862568757793de76c3359`, WASM
`dbdbe10bb5da42c560ecf1616b802eacb165274ed251fb0679a59a8e5223f239`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, and
`index.html` `70cd34d6459f3db983bfa1790b4dba6085accb6f5549ea7d377c5824179dccff`.

Observation: the candidate armed
`native ntdll RtlWakeAddressAll empty-bucket fast path @ 3f949570`; it locks
the exact NTDLL bucket, returns natively only for an empty waiter list, and
falls back for contended/non-empty buckets. A 30-second enabled run reached
`FPSSAMPLE t=39.1 flips=24637`; the matched control with
`WASM_NO_NTDLL_WAKE_FAST=1` reached `t=41.3 flips=23330`. The runs were noisy
but directionally favorable (about 12% more flips over their warm windows).
Both runs rendered changing 640x400 frames, accepted input/audio, and had no
`FATAL`, `RuntimeError`, `JITBAD`, or `JITBADEIP`.

Decision: promote the guarded empty-bucket path and continue profiling the
remaining non-empty wake and exclusive-release misses.

Canonical regression: the exact URL above armed the same hook, reached
`FPSSAMPLE t=20.8 flips=10370`, and produced changing 640x400 frames with
input ready and no fatal/JIT errors.

## 2026-08-31 01:51 IDT: removed shared-SRW release fast path after black-screen report

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the dirty `vibe` tree is
still at `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`, with unrelated dirty and
untracked work preserved. Served artifacts are JS
`a21c150edb0bf61b58c2749daf04b952d4b896a63beb01afc7279af9a58311e9`, WASM
`1a68330fc0791cfc32c0a130a4b66cc8c094bdeea6f7a1da40d5adb6b815044c`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, and
`index.html` `70cd34d6459f3db983bfa1790b4dba6085accb6f5549ea7d377c5824179dccff`.

Observation: the newest `RtlReleaseSRWLockShared` shortcut was removed from
the source and the browser bundle was rebuilt and promoted. A fresh canonical
run reached a visible 640x400 Duke Nukem frame, accepted two input keys, and
logged no `FATAL`, `RuntimeError`, `JITBAD`, or `JITBADEIP`. The captured frame
was non-black and changing; the late run reached `FPSSAMPLE t=24.7 flips=10246`
and about 949 fps. The retained exclusive/shared acquire paths were still
armed.

Decision: keep shared release interpreted until the reported black-screen
case is disproven across the user's browser; ask the user to reload the
canonical URL so the rollback bundle is fetched.

## 2026-08-31 01:47 IDT: promoted shared-SRW release fast path

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the dirty `vibe` tree is
still at `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`, with unrelated dirty and
untracked work preserved. Served artifacts are JS
`a21c150edb0bf61b58c2749daf04b952d4b896a63beb01afc7279af9a58311e9`, WASM
`71757ce7cb3b918f41e9e0f46a4f70b885e8759b8d5a8436bcbdee149092759d`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, and
`index.html` `70cd34d6459f3db983bfa1790b4dba6085accb6f5549ea7d377c5824179dccff`.

Observation: the candidate armed
`RtlAcquireSRWLockExclusive @ 3f928480`,
`RtlAcquireSRWLockShared @ 3f928540`, and
`RtlReleaseSRWLockShared @ 3f944100`. Its control used
`WASM_NO_NTDLL_SRW_FAST=1`. Both arms reached changing 640x400 frames, input
ready, and no `FATAL`, `RuntimeError`, `JITBAD`, or `JITBADEIP`; the canonical
regression reached `FPSSAMPLE t=26.2 flips=6553` with the same clean status.
The matched steady samples were noisy, so no fixed FPS percentage is claimed.
The release shortcut handles only reader-count >=2 with no low-half waiter or
exclusive bits; final-reader and wake cases remain interpreted.

Decision: promote the guarded shared-release path and continue with the
remaining NTDLL wake/release cluster.

## 2026-08-31 01:42 IDT: shared-SRW canonical regression and next hotspot

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the dirty `vibe` tree is
still at `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`, with unrelated dirty and
untracked work preserved. Canonical artifacts are JS
`a21c150edb0bf61b58c2749daf04b952d4b896a63beb01afc7279af9a58311e9`, WASM
`1a68330fc0791cfc32c0a130a4b66cc8c094bdeea6f7a1da40d5adb6b815044c`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, and
`index.html` `70cd34d6459f3db983bfa1790b4dba6085accb6f5549ea7d377c5824179dccff`.

Observation: a fresh canonical diagnostic run reached changing 640x400
frames, with input ready and no `FATAL`, `RuntimeError`, `JITBAD`, or
`JITBADEIP`. The post-promotion profile still identifies the largest
interpreted clusters as `3f9440xx`/`3f9495xx` (NTDLL SRW release/wake paths),
while the repeated `3ee39b80` entries are the already guarded MSVCRT memset
loop and `3ee3b5xx` is startup locale construction. The control run with
`WASM_NO_NTDLL_SRW_FAST=1` also rendered cleanly; its steady rates were within
normal run-to-run variance, so no fixed FPS percentage is claimed.

Decision: retain the exact shared-acquire promotion and map the NTDLL wake
path before attempting another synchronization shortcut.

## 2026-08-31 01:35 IDT: promoted shared SRW acquire fast path

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the dirty `vibe` tree is
still at `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`, with unrelated dirty and
untracked work preserved. Served artifacts are JS
`a21c150edb0bf61b58c2749daf04b952d4b896a63beb01afc7279af9a58311e9`, WASM
`1a68330fc0791cfc32c0a130a4b66cc8c094bdeea6f7a1da40d5adb6b815044c`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, and
`index.html` `70cd34d6459f3db983bfa1790b4dba6085accb6f5549ea7d377c5824179dccff`.

Observation: the candidate armed
`RtlAcquireSRWLockExclusive @ 3f928480` and
`RtlAcquireSRWLockShared @ 3f928540`; the control used
`WASM_NO_NTDLL_SRW_FAST=1`. Both runs produced changing 640x400 frames,
reported input ready, and had no `FATAL`, `RuntimeError`, `JITBAD`, or
`JITBADEIP`. The short run did not isolate a statistically reliable FPS delta,
so no percentage gain is claimed. The shared path only CAS-increments the
shared-count half-word when the exclusive/waiter half-word is zero; all other
states fall back to Wine.

Decision: promote the guarded shared-acquire path and continue measuring the
next hotspot.

## 2026-08-31 01:20 IDT: rejected SRW exclusive-release shortcut

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the dirty `vibe` tree is
still at `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`, with unrelated dirty and
untracked work preserved. The canonical acquire-only WASM remains
`42b7c75815b6eb219323016dd1ad225a67ff693f00dd5aebc70c95cab5a6579c`; the
isolated release candidate was `b4099a0da5e372f3d3a72ade78aec06e74cfb8e8f46481eb3fc44f44943d96b2`.

Observation: the candidate armed both
`RtlAcquireSRWLockExclusive @ 3f928480` and
`RtlReleaseSRWLockExclusive @ 3f944060`, rendered changing 640x400 frames,
accepted input, and produced no `FATAL`, `RuntimeError`, `JITBAD`, or
`JITBADEIP`. Its control used `WASM_NO_NTDLL_SRW_FAST=1`. Across the matched
runs, steady guest flip rates varied more than the enabled/control difference;
the release-only `state 1 -> 0` shortcut therefore has no reliable measured
FPS gain.

Decision: remove and reject the release shortcut. Keep the previously verified
acquire-only path canonical and continue profiling another target.

## 2026-08-31 01:13 IDT: promoted uncontended ntdll SRW-lock fast path

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the dirty `vibe` tree is
still at `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`, with unrelated dirty and
untracked work preserved. Served artifacts are JS
`a21c150edb0bf61b58c2749daf04b952d4b896a63beb01afc7279af9a58311e9`, WASM
`42b7c75815b6eb219323016dd1ad225a67ff693f00dd5aebc70c95cab5a6579c`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, and
`index.html` `70cd34d6459f3db983bfa1790b4dba6085accb6f5549ea7d377c5824179dccff`.

Observation: the canonical regression logged
`native ntdll RtlAcquireSRWLockExclusive fast path @ 3f928480`, initialized
OpenGL, accepted input, and produced changing 640x400 frames (`distinct
frames: 4`) with no `FATAL`, `RuntimeError`, `JITBAD`, or `JITBADEIP`. The
matched candidate control used `WASM_NO_NTDLL_SRW_FAST=1`; both arms rendered
cleanly. In later steady windows the enabled run sampled about 1.0–1.1K guest
flips/s and the control about 1.0–1.05K; browser/game-state timing is noisy, so
no fixed percentage is claimed. The hook declines waiter/contended states and
the kill switch remains available.

Decision: promote the guarded CAS-only path. The rebuilt WASM also removes the
stale rejected SDL global-getter code from the served artifact.

## 2026-08-31 00:57 IDT: fixed stale worker bootstrap on black canvas

The canonical URL remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the dirty `vibe` tree is
still at `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`, with unrelated dirty and
untracked work preserved. The served JS hash is
`a21c150edb0bf61b58c2749daf04b952d4b896a63beb01afc7279af9a58311e9`, WASM
`73c6411b44103038e0771b85b446ea18d046929acc08d2b6c4a6140e003dcbd0`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, and the
updated `index.html` hash is
`70cd34d6459f3db983bfa1790b4dba6085accb6f5549ea7d377c5824179dccff`.

Observation: the page now creates `worker.js?boot=<timestamp>` as well as
loading `webwine-bw.js?boot=<timestamp>`, pairing an existing tab's worker with
the current bundle. A fresh canonical run reached a changing 640x400 frame in
8.8s, reported `input: ready`, and had no `FATAL`, `RuntimeError`, `JITBAD`, or
`JITBADEIP`. The initial canvas is black until the first frame; a separate run
also showed an observed 6–7s no-present interval during game-state transition,
after which frame hashes changed again. This is recorded as an observation,
not treated as a crash hypothesis.

Decision: promote the worker cache-bust in the served page. Hard-refresh the
canonical URL once if an old tab is currently open.

## 2026-08-31 00:50 IDT: rejected SDL global getter fast path

The canonical URL remained
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the dirty `vibe` tree is
still at `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`, with unrelated dirty and
untracked work preserved. The canonical artifact was not changed: JS
`a21c150edb0bf61b58c2749daf04b952d4b896a63beb01afc7279af9a58311e9`, WASM
`73c6411b44103038e0771b85b446ea18d046929acc08d2b6c4a6140e003dcbd0`, and
data `d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: the live diagnostic dump mapped `0x0070d7a0` to the exact body
`a1 28 bd ac 01 c3` (`mov eax,[0x01acbd28]; ret`). An isolated candidate
armed a range-checked native getter and its control disabled it with
`WASM_NO_SDL_GLOBAL_GET=1`. Both arms rendered 640x400 frames and produced no
`FATAL`, `RuntimeError`, `JITBAD`, or `JITBADEIP`; the enabled arm logged
`native SDL global getter @ 0070d7a0`. The runs were dominated by the same
menu-load stall and showed no reliable sustained FPS improvement.

Decision: remove and reject this neutral optimization. The `0x0070d7a0`
getter remains interpreted; continue with a different measured hotspot.

## 2026-08-31 00:43 IDT: promoted SDL atomic-add fast path

Profiled and tested the canonical URL
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1` and an isolated candidate
with `WASM_NO_SDL_ATOMIC_XADD=1` as its control. The dirty `vibe` tree remains
at `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`, with unrelated dirty and
untracked work preserved. Canonical artifacts are JS
`a21c150edb0bf61b58c2749daf04b952d4b896a63beb01afc7279af9a58311e9`, WASM
`73c6411b44103038e0771b85b446ea18d046929acc08d2b6c4a6140e003dcbd0`, and
data `d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: `WASM_DUMP_ADDR=0x73cab0` showed the exact SDL atomic-add body
`8b 54 24 04 8b 44 24 08 f0 0f c1 02 c3`. The guarded native handler at
`0x0073cab0` uses a sequentially consistent host fetch-add, returns the old
value, and declines outside guest memory. Enabled and control runs both
rendered 640x400 frames with no `FATAL`, `RuntimeError`, `JITBAD`, or
`JITBADEIP`; the longer enabled run reached `14562` guest flips and the
control `13356` over comparable sustained windows. The browser timing remains
noisy, so no fixed FPS percentage is claimed. The canonical regression armed
`native SDL_AtomicAdd_REAL @ 0073cab0` and reached `FPSSAMPLE t=13.4 flips=5415`
before the normal menu-load stall, with no runtime error.

Decision: promote the exact, range-checked xadd fast path. Its kill switch is
`WASM_NO_SDL_ATOMIC_XADD=1` if a future platform needs the interpreter path.

## 2026-08-31 00:27 IDT: rejected SDL atomic veneer aliases

Profiled the promoted canonical URL
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1&WASM_IPAGE=1&WASM_MODULES=1`.
The dirty `vibe` tree remains at
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`, with unrelated dirty and
untracked work preserved. The canonical artifact remains JS
`a21c150edb0bf61b58c2749daf04b952d4b896a63beb01afc7279af9a58311e9`, WASM
`633c32319bc87fd5f61f98132f988b0f3b4bf60666ec281a7d5653530808d0fe`, and
data `d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: the profile's `IMISS top` showed `0073cab8`, `0073cab0`,
`0073cabc`, and `0073cab4` at about 0.7M each, immediately before the
currently armed `SDL_AtomicGet_REAL @ 0073cac0`. An opt-in candidate added
byte-guarded aliases for those four aligned addresses, but its rebuilt pair
failed before Wine startup with
`WebAssembly.instantiate(): Import #0 "env": module is not an object or
function`. The canonical URL continued to render changing 640x400 frames
while this candidate was tested; no candidate artifact was promoted.

Decision: removed the alias experiment and retained the verified canonical
atomic getter build. The adjacent addresses need a clean symbol/veneer map
before another native dispatch attempt.

## 2026-08-31 00:19 IDT: promoted browser cache/startup fix

The canonical URL is
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1` (port 8799 remains the
alias). The dirty `vibe` tree remains at
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`, with unrelated dirty and
untracked work preserved. Canonical artifacts are JS
`a21c150edb0bf61b58c2749daf04b952d4b896a63beb01afc7279af9a58311e9`, WASM
`633c32319bc87fd5f61f98132f988b0f3b4bf60666ec281a7d5653530808d0fe`, and
data `d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: the worker now imports the wasm module with a per-boot cache-bust
query, and the page reports `waiting for first frame (the initial load can
take about 10s)` instead of appearing silently black. The isolated candidate
and then the canonical URL both loaded the supplied executable, initialized
the 640x400 renderer, logged `native SDL_AtomicGet_REAL @ 0073cac0`, and
produced changing non-black frames (`FPSSAMPLE t=15.6 flips=3997` in the
canonical check). Input was ready; no `BADIP`, `JITBAD`, `JITBADEIP`, `FATAL`,
or `RuntimeError` occurred. The initial black canvas during asset/JIT startup
is therefore expected, not a rendering failure.

Decision: promote the cache-busted worker and startup status alongside the
verified wasm bundle. Open the exact URL above and allow the initial load to
finish; a stale tab should now fetch a fresh worker/module pair on reload.

## 2026-08-30 23:49 IDT: rejected `setupmvlineasm` shortcut

Profiled the canonical URL
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1&WASM_IPAGE=1&WASM_MODULES=1`
and identified `0x00631c60` as the next startup/mapper setup hotspot. An
isolated candidate at `http://localhost:8919/?WASM_TPUT=1&WASM_BADIP=1`
added a byte-verified native implementation of its five SMC byte writes.
The dirty `vibe` tree remains
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`, with unrelated dirty and
untracked work preserved. The canonical artifact remains JS
`ad9ff2fa59d9619f6d2d959392cc6ecc3d4b4efa19f25458115ede6b9bda0ef5`, WASM
`15c2cfd8805aaef3ccb5315de1bbd23d0dd8f3b5a4a503cb894ecbe5e1ff8286`, and
data `d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: the candidate armed `native setupmvlineasm @ 00631c60` but
stalled before sustained rendering, reaching only `FPSSAMPLE t=32.7 flips=25`
with no real frame. It was not promoted. The canonical server was not
modified and remains the last verified `mvlineasm1` build.

Decision: remove the shortcut from source and reject this experiment. The
profile’s next target must be selected after the canonical build is restored,
with SMC setup side effects treated as correctness-critical.

## 2026-08-30 23:39 IDT: promoted true-entry `mvlineasm1` hook

Mapped the post-startup profile hotspot `0x00631d90–0x00631dae` to the
five-push/six-load prologue of the masked single-column mapper. Added a
guarded true-entry handler that reconstructs the original stack frame and
register arguments before reusing the verified `mvlineasm1` loop. The
candidate URL was `http://localhost:8918/?WASM_TPUT=1&WASM_BADIP=1`; the
canonical URL tested after promotion was
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1` (8799 remains the alias).
The dirty `vibe` tree remains
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`, with unrelated dirty and
untracked work preserved. Promoted hashes are JS
`ad9ff2fa59d9619f6d2d959392cc6ecc3d4b4efa19f25458115ede6b9bda0ef5`, WASM
`15c2cfd8805aaef3ccb5315de1bbd23d0dd8f3b5a4a503cb894ecbe5e1ff8286`, and
data `d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: the candidate logged `native mvlineasm1 @ 00631d90` and reached
`FPSSAMPLE t=24.7 flips=8886`; its same-artifact control with
`WASM_NO_MVLINE1_ENTRY=1` reached `t=24.5 flips=8699`. Both had changing
640x400 frames, input ready, audio on, and no `BADIP`, `JITBAD`, `JITBADEIP`,
`FATAL`, or `RuntimeError`. The canonical regression logged all four mapper
hooks plus the MSVCRT loop and reached `FPSSAMPLE t=20.8 flips=5979` with
changing frames, input/audio, and no runtime errors. The gain is modest, so
no fixed FPS percentage is claimed.

Decision: retain the true-entry hook and continue profiling the remaining
non-power-of-two/single-column mapper residue.

## 2026-08-30 23:29 IDT: post-hook profile and MSVCRT timing trace

Ran the post-promotion diagnostic at
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1&WASM_IPAGE=1&WASM_MODULES=1`
and an isolated trace at
`http://localhost:8917/?WASM_TPUT=1&WASM_BADIP=1&WASM_TRACE_MSVCRT_HOT=1`.
The dirty `vibe` tree remains
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`, with unrelated dirty and
untracked work preserved. The canonical artifact hashes remain JS
`ad9ff2fa59d9619f6d2d959392cc6ecc3d4b4efa19f25458115ede6b9bda0ef5`, WASM
`9747077c3a81ed0a5fe999af49c1f6cef4ec314c036937d16f0cfdaafd7cac58`, and
data `d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: the diagnostic canvas reached changing 640x400 frames with
input ready/audio on and no `BADIP`, `JITBAD`, `JITBADEIP`, `FATAL`, or
`RuntimeError`. The trace showed the early MSVCRT `memset` entry executing
before its late module hook is armed, then the verified internal loop at
`3ee39b80` with `ESI`/`EBX` guest-range state. The repeated `IMISS top`
entries in this profile therefore include startup misses and are not a
post-arm measure of the loop hook; the isolated enabled/disabled A/B remains
the authoritative performance comparison.

Decision: retain the promoted loop hook and avoid broadening it based on the
startup profile. The next target remains the post-arm CRT/string residue or
the SMC single-column mapper, selected with a profile that starts after module
initialization.

## 2026-08-30 23:26 IDT: promoted guarded MSVCRT `memset` SIMD-loop hook

Mapped the repeated `3ee39b80` misses to the verified MSVCRT `memset` SIMD
loop and promoted the guarded native loop handler to the canonical artifact.
The candidate was tested at
`http://localhost:8916/?WASM_TPUT=1&WASM_BADIP=1`; the canonical URL tested
after promotion was `http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1` (8799
remains the alias). The dirty `vibe` tree is still
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`, with unrelated dirty and
untracked work preserved. Promoted hashes are JS
`ad9ff2fa59d9619f6d2d959392cc6ecc3d4b4efa19f25458115ede6b9bda0ef5`, WASM
`9747077c3a81ed0a5fe999af49c1f6cef4ec314c036937d16f0cfdaafd7cac58`, and
data `d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: the candidate logged native handlers at `006320b0`,
`006324d0`, `3ee39b80`, and `3ee4ff40`, and its isolated A/B reached
`FPSSAMPLE t=21.6 flips=5462` with the loop enabled versus
`t=21.0 flips=4133` with only `WASM_NO_MEMSET_LOOP=1`. The canonical
regression logged the same handlers and reached
`FPSSAMPLE t=22.9 flips=5625`, with a changing 640x400 canvas, input ready,
audio on, and no `BADIP`, `JITBAD`, `JITBADEIP`, `FATAL`, or `RuntimeError`.

Decision: retain the loop hook. The next profile should reassess the now
exposed CRT/string and single-column mapper residue.

## 2026-08-30 23:01 IDT: promoted true-entry `vlineasm4` dispatcher hook

Moved the unmasked dispatcher hook from its delayed setup address to the true
entry at `0x006320b0`, reproducing its three initial register pushes under the
same byte skeleton guard. The candidate was tested at
`http://localhost:8915/?WASM_TPUT=1&WASM_BADIP=1` and promoted to
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; port 8799 remains the alias.
The dirty `vibe` tree is still
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`, with unrelated dirty and
untracked work preserved. Promoted hashes are JS
`ad9ff2fa59d9619f6d2d959392cc6ecc3d4b4efa19f25458115ede6b9bda0ef5`, WASM
`38bc02f43027e481ece5b1c9ef6c6662bf5d75eef5a9cf79040c01abed43f914`, and
data `d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: the final canonical regression logged both
`native vlineasm4 dispatcher @ 006320b0` and
`native mvlineasm4 dispatcher @ 006324d0`; it reached
`FPSSAMPLE t=21.8 flips=5438` with a changing 640x400 canvas, input ready,
audio on, and no `BADIP`, `JITBAD`, `JITBADEIP`, `FATAL`, or `RuntimeError`.
The isolated candidate likewise rendered changing frames and reached
`FPSSAMPLE t=21.2 flips=5768`. The candidate is retained as a measured
positive direction, without claiming a fixed FPS percentage because browser
startup/stalls are variable.

Decision: keep both mapper dispatchers at their true entries. The next
optimization target is the remaining CRT/internal mapper residue shown by the
post-promotion profile.

## 2026-08-30 22:50 IDT: promoted true-entry `mvlineasm4` dispatcher hook

The guarded dispatcher optimization was built from the dirty `vibe` tree at
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b` and tested first at
`http://localhost:8914/?WASM_TPUT=1&WASM_BADIP=1`, then promoted to the
canonical URL `http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1` (port 8799
is the same alias). Unrelated dirty and untracked work remains preserved.
The promoted hashes are JS
`ad9ff2fa59d9619f6d2d959392cc6ecc3d4b4efa19f25458115ede6b9bda0ef5`, WASM
`45085c5f9b59019aed73b1386ea97305878aeab0da0e34cb1764205de551511e`, and
data `d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: the candidate logged `native mvlineasm4 dispatcher @ 006324d0`
and `native __udivmoddi4 @ 3ee000f0`; it reached
`FPSSAMPLE t=34.7 flips=14901` with no `BADIP`, `JITBAD`, `JITBADEIP`,
`FATAL`, or `RuntimeError`. CDP saw a changing 640x400 canvas, input ready,
and audio on. The final canonical regression reached
`FPSSAMPLE t=25.4 flips=7431`; CDP reported a changing 640x400 canvas,
first-frame at 15.8s, 189 frames at its 20s sample, input ready, and audio
on, with no runtime errors. The pre-patch control had a dispatcher-heavy
stall (`FPSSAMPLE t=17.4 flips=2693`, then `t=30.7 flips=5594`), whereas the
candidate reached `t=30.6 flips=12190`; timing varies, but this is a strong
positive signal for the true-entry hook.

Decision: keep the promotion. The former `0x006324d0` interpreted entry miss
is removed; the next profile should verify whether the remaining hot misses
are the CRT paths or the single-column mapper.

## 2026-08-30 22:34 IDT: native divide hotspot cleared; next residue is mapper/CRT dispatch

Ran fresh 30-second matched tests against the canonical URL
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1` and the same-build control
with `WASM_NO_UDIV_NATIVE=1`. The dirty `vibe` tree is still
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; unrelated dirty and untracked
work remains preserved. The canonical hashes remain JS
`6a37ec40318acbed1cbca5c940bf783113f659add00b44617f095faaef391d08`, WASM
`7ccb5516808dfeb071ac1c6c6ef0c8b207fe9f6a941f9dde506f8d6c94564ca7`, and
data `d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: the enabled run logged `native __udivmoddi4 @ 3ee000f0`,
finished at `FPSSAMPLE t=30.0 flips=11172`, and CDP reported a changing
640x400 canvas, first frame at 13.2s, 546 frames, input ready, and audio on.
The control finished at `FPSSAMPLE t=30.4 flips=10522` and CDP reported a
changing 640x400 canvas, first frame at 14.5s, 515 frames, input ready, and
audio on. Neither run produced `BADIP`, `JITBAD`, `JITBADEIP`, `FATAL`, or
`RuntimeError`. The difference is positive but not clean enough to claim a
precise FPS uplift.

The enabled diagnostic profile no longer shows the former `3ee00000`
divide-helper page in `IMISS top`. The sustained residue is now led by the
`mvlineasm4` dispatcher (`006324d0/006320b0`) and CRT string/memory paths
(`3ee39b8x`, `3ee4ffxx`), with the single-column mapper counters active.

Decision: retain the guarded native helper and investigate the dispatcher and
remaining mapper/CRT misses as the next measured optimization targets.

## 2026-08-30 22:31 IDT: promoted native `__udivmoddi4` to canonical bundle

Promoted the corrected candidate to the canonical artifact root and restarted
ports 8806 and 8799. The exact canonical URL tested was
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`. The dirty `vibe` tree is
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; unrelated dirty and untracked
work remains preserved. Both ports now serve JS
`6a37ec40318acbed1cbca5c940bf783113f659add00b44617f095faaef391d08`, WASM
`7ccb5516808dfeb071ac1c6c6ef0c8b207fe9f6a941f9dde506f8d6c94564ca7`, and
data `d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: the shipped run logged `native __udivmoddi4 @ 3ee000f0`, reached
a changing 640x400 WebGL canvas with input/audio, and produced no
`BADIP`, `JITBAD`, `JITBADEIP`, `FATAL`, or `RuntimeError`. Its final guest
sample was `FPSSAMPLE t=25.0 flips=8374`; CDP reported six non-black samples,
four distinct frames, and input ready/audio on. The native helper’s same-build
controls also rendered successfully. The candidate is directionally faster in
the matched samples, but browser timing remains noisy; no claim of a precise
percentage is made.

Decision: keep the native helper in the served build. The next benchmark
should use the canonical URL above and compare multiple fresh runs with
`WASM_NO_UDIV_NATIVE=1` as the control.

## 2026-08-30 22:28 IDT: corrected native `__udivmoddi4` candidate is stable

Built and tested the Wine-only candidate at
`http://localhost:8912/?WASM_TPUT=1&WASM_BADIP=1`; the dirty `vibe` tree is
still `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`, with unrelated dirty and
untracked work preserved. Candidate hashes were JS
`6a37ec40318acbed1cbca5c940bf783113f659add00b44617f095faaef391d08`, WASM
`7ccb5516808dfeb071ac1c6c6ef0c8b207fe9f6a941f9dde506f8d6c94564ca7`, and
data `d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: the log confirms `native __udivmoddi4 @ 3ee000f0`; two runs
completed with changing 640x400 WebGL frames, input ready, audio on, and no
`BADIP`, `JITBAD`, `JITBADEIP`, `FATAL`, or `RuntimeError`. The corrected
candidate reached `FPSSAMPLE t=26.0 flips=9075` on the repeat; its same-build
control with `WASM_NO_UDIV_NATIVE=1` reached `t=24.6 flips=6203` in a run
with a long interpreter stall. The earlier candidate/control pair showed
`flips=7493` at `t=24.4` versus `7123` at `t=24.2`. This is encouraging but
not a clean standalone FPS benchmark because startup and stall timing vary.

Decision: the hook is stable and directionally beneficial in repeated runs;
promote it to the canonical build, then run a fresh canonical regression.

## 2026-08-30 21:59 IDT: rejected native MSVCRT divide helper

Tested a guarded native implementation of the traced non-exported MSVCRT
`__udivmoddi4` at
`http://localhost:8910/?WASM_TPUT=1&WASM_BADIP=1`. The dirty `vibe` tree
remains at `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`, with unrelated dirty
and untracked work preserved. Candidate hashes were JS
`98f9223cdcf0a017492e8a56d51362be9dc60bf856fb33affbe6aea360c047b3`, WASM
`a162d41c212d3cb9d7f832bc06067e16a27b625430bead64090b848563a91c40`, and
data `d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: the candidate armed (`native __udivmoddi4 @ 3ee000f0`) but
failed during startup with `FATAL worker exception: memory access out of
bounds`, before the first frame. No candidate frame or input result is valid.
The native divide implementation and its relocation initialization were
removed; canonical ports 8806 and 8799 remain on the prior verified artifact.

Decision: do not promote this helper without a dedicated ABI/stack trace and
guest-memory validity proof. It is not a performance result. Return to the
measured renderer residue for the next optimization.

## 2026-08-30 21:37 IDT: rejected narrow MSVCRT divide-helper AOT

Tested a focused AOT table covering the traced MSVCRT compiler helpers at
`http://localhost:8908/?WASM_TPUT=1&WASM_BADIP=1`; the exact table range was
`0x100100f0-0x10010b80` and translated 143/143 blocks. The dirty `vibe` tree
remains at `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`, with unrelated dirty
and untracked work preserved. Candidate hashes were JS
`c4d26dc8fd04b2000731f9b6b23d9e155d1ee007964a826398e1434a9cecdf21`, WASM
`83d91a02c4a06b212ea68567a276479d4cdf650684cd2d5d4177058200af2d4c`, and
data `d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: the candidate loaded `msvcrt JIT 143 translated blocks` and
rendered a changing 640x400 WebGL canvas with input/audio and no
`BADIP`, `JITBAD`, `JITBADEIP`, `FATAL`, or `RuntimeError`. Its final sample
was `t=28.4 flips=8267`; the same-artifact control with
`WASM_NO_MSVCRT_JIT=1` reached `t=23.6 flips=10229`. The candidate was
therefore substantially slower and was not promoted. The temporary candidate
server was stopped; canonical ports 8806 and its 8799 alias are unchanged.

Decision: translating this initialization/compiler-helper range does not
improve the measured frame workload. The next optimization should return to
the sustained renderer residue (especially the SMC single-column mapper or a
measured SSE/x87 loop), rather than adding broad MSVCRT AOT.

## 2026-08-30 21:20 IDT: traced MSVCRT-range misses; canonical render confirmed

Built an isolated current-source O2 diagnostic bundle from the dirty `vibe`
tree at `ad816e0a7dd68ee9030b0e8d89967e82a79f325b` and tested
`http://localhost:8907/?WASM_TPUT=1&WASM_BADIP=1&WASM_TRACE_MSVCRT_MISS=1`.
Candidate hashes were JS
`f7a36d4255964ce2b1d4e5198e48291323a1288e57b2ba5538041a7183562086`, WASM
`cb713461878a3d3bc681fa955b360ab3fd86a83ee95229dfc6003114e08ea146`, and
data `d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: the trace shows executable bytes at the loaded MSVCRT base, not
an inert PE header. For example, `eip=3ee00b10` begins
`55 89 e5 53 8b 1d ...`, and `eip=3ee000f0` begins
`55 89 e5 57 56 53 ...`; the trace also reports callers such as
`ret=3ee7c45c` and `ret=3ee41d14`. The run reached a changing 640x400
WebGL canvas, input ready, audio on, and `FPSSAMPLE t=23.3 flips=7573` with
no `BADIP`, `JITBAD`, `JITBADEIP`, `FATAL`, or `RuntimeError`.

The canonical server was independently checked at
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`: it also reached a changing
640x400 canvas, input/audio, and `FPSSAMPLE t=26.0 flips=10217`. Port 8799 was
restarted as an alias to the same verified bundle, so the earlier advertised
URL is usable again; port 8806 remains canonical.

Hypothesis: the apparent MSVCRT “header” misses are relocated executable
thunks/functions and need symbol/ABI mapping before optimization; no native
hook was promoted from this trace. For testing, use port 8806 and allow the
roughly 10-second startup/asset-loading period before judging the initially
black canvas.

## 2026-08-30 21:08 IDT: mapped current miss profile

Re-ran the canonical diagnostic at
`http://localhost:8806/?WASM_TPUT=1&WASM_PROF=1&WASM_BADIP=1`. The dirty
`vibe` tree remains at `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; unrelated
dirty and untracked work is preserved. Port 8806 serves JS
`8487e134d32973363c82d295a8513663b30f584100813e31d3f0c8c799b31e5c`, WASM
`9b1e43918bed3f9fd8c58c54cf1073d2768702153239ea32bd807f83b682148e`, and
data `d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: the run reached a real changing 640x400 WebGL canvas, input
ready, and audio on before a later timing stall. The miss profile again led
with MSVCRT addresses `0x3ee002xx` (~0.7M each), while the earlier mapper
dispatch entries were not the dominant misses in this run. The addresses are
inside the module-header region rather than the PE `.text` RVA, so they are
not safe native-hook targets without first tracing the return/thunk mapping.
No `BADIP`, `JITBAD`, `JITBADEIP`, `FATAL`, or `RuntimeError` was observed.

Next action: add a narrowly scoped miss-byte/return-address diagnostic and
identify the owning thunk before attempting another performance change.

## 2026-08-30 21:06 IDT: current O2 hotspot profile after rollback

Profiled the restored canonical artifact at
`http://localhost:8806/?WASM_TPUT=1&WASM_IPAGE=1&WASM_MODULES=1&WASM_BADIP=1`.
The dirty `vibe` tree remains at
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; unrelated dirty and untracked
work is preserved. Port 8806 serves JS
`8487e134d32973363c82d295a8513663b30f584100813e31d3f0c8c799b31e5c`, WASM
`9b1e43918bed3f9fd8c58c54cf1073d2768702153239ea32bd807f83b682148e`, and
data `d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: the run reached a real changing 640x400 WebGL canvas, input
ready, audio on, and no `BADIP`, `FATAL`, `JITBAD`, `JITBADEIP`, or
`RuntimeError`. At the sustained sample, `FPSSAMPLE t=25.7 flips=11986`;
the miss profile was led by MSVCRT `0x3ee002xx` entries (~0.7M each), then
NTDLL `0x3f94xxxx`/`0x3f92xxxx` pages. Mapper dispatcher misses at
`0x6324d0` were also visible, but the true-entry alias A/B was slower.

Next action: map the `0x3ee002xx` addresses to their actual loaded thunk
ownership and test a narrowly guarded helper only if its ABI and return path
can be proven; do not translate the PE-header-looking range blindly.

## 2026-08-30 21:04 IDT: rejected mapper true-entry aliases

The current profile showed repeated misses at the true mapper prologue
entries (`0x6320b0` and `0x6324d0`), while native hooks were registered after
their initial pushes. Tested registering those true entries and emulating the
missing pushes before entering the existing verified native handlers at
`http://localhost:8906/?WASM_TPUT=1&WASM_IPAGE=1&WASM_MODULES=1&WASM_BADIP=1`.
The dirty `vibe` tree remains at
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; unrelated dirty and untracked
work is preserved. Candidate hashes were JS
`476313f69a08fc59c1088b598b6891b51f467219d4b843e690e1132debc0f919`, WASM
`691557bae35679afb8fb24f8efb4864f07d01b22472023b1fd2fcc347b54d57a`, and
data `d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: the candidate removed the mapper miss cluster and rendered a
real changing 640x400 WebGL canvas with input/audio and no runtime errors,
but its matched run reached `flips=9420` at guest `t=24.4`; canonical reached
`9813` at `t=24.3`.

Decision: reverted the true-entry aliases. Canonical port 8806 remains on the
verified mapper hooks and O2 generated-block build.

## 2026-08-30 20:50 IDT: rejected XOPT=-O3 and restored O2 canonical

Tested an isolated browser build with the interpreter compiled at `XOPT=-O3`
on `http://localhost:8905/?WASM_TPUT=1&WASM_BADIP=1`; its candidate hashes
were JS `4b0049f5f7e28df9f9aa58caa103a33179496f2db4d5254d86b64abf3cb63b9a`,
WASM `744cfef698a6689f40e4d0c6f31ec3a53905307e88d5c941fd41afd9bac8f4fa`,
and data `d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.
The dirty `vibe` tree remains at
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; unrelated dirty and untracked
work is preserved.

Observation: the first candidate run reached `flips=10605` at guest `t=22.8`,
but a rerun reached only `9150` at `t=22.7`. The contemporaneous O2 controls
reached `9657` at `t=23.2` and, after restoring O2, `8257` at `t=23.0`.
Every run that reached gameplay had a real changing 640x400 WebGL canvas,
input ready, audio on, and no `BADIP`, `FATAL`, `JITBAD`, `JITBADEIP`, or
`RuntimeError`. The positive first run was timing noise, so O3 was rejected.

Decision: rebuilt and restored the canonical O2 bundle. Port 8806 now serves
JS `8487e134d32973363c82d295a8513663b30f584100813e31d3f0c8c799b31e5c`, WASM
`9b1e43918bed3f9fd8c58c54cf1073d2768702153239ea32bd807f83b682148e`, and
data `d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

## 2026-08-30 20:26 IDT: rejected bulk SSE primitive fast path

Tested replacing the translated SSE primitive’s byte-at-a-time 128-bit
loads/stores with guarded `memcpy` calls at
`http://localhost:8904/?WASM_TPUT=1&WASM_BADIP=1`; the same-artifact control
used `WASM_NO_FAST_XMM=1`. The dirty `vibe` tree remains at
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`, with unrelated dirty and
untracked work preserved. Candidate hashes were JS
`6241c19005728f27c1577a6844537ea250f142c9ae00c026f88c78a95e7676b4`, WASM
`c028acc543c9807da20d84ba7f1ad67b402ba93f70ce402081a4baebd3a86409`, and
data `d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: both runs reached real changing 640x400 WebGL frames, input
ready, audio on, and no `BADIP`, `FATAL`, `JITBAD`, `JITBADEIP`, or
`RuntimeError`. Enabled reached `flips=10397` at guest `t=23.6`; control
reached `flips=10396` at `t=23.2`, which is indistinguishable from timing
noise.

Decision: reverted the fast SSE primitive path. Canonical port 8806 remains
unchanged.

## 2026-08-30 19:58 IDT: rejected entry-rooted MSVCRT locale AOT

Tested a focused AOT table rooted at MSVCRT `create_locinfo` (`0x1003aba0`)
through `0x1003f000` at
`http://localhost:8903/?WASM_TPUT=1&WASM_BADIP=1&WASM_MSVCRT_JIT=1`; the
same-artifact control omitted `WASM_MSVCRT_JIT`. The table translated 864
blocks. The dirty `vibe` tree remains at
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`, with unrelated dirty and
untracked work preserved. Candidate hashes were JS
`0aaa30141958725ab92dcd5e6b8bf40d441568f3ecff7b8f9c4c75c59cbe29f3`, WASM
`1c8796d2b343996a7c87162591b380a30f22d5813e1fddf134ad70d35da2ef07`, and
data `d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: both runs reached real changing 640x400 WebGL frames, input
ready, audio on, and no `BADIP`, `FATAL`, `JITBAD`, `JITBADEIP`, or
`RuntimeError`. Enabled reached `flips=8373` at guest `t=24.3`; control
reached `flips=10603` at `t=23.7`. The broader function translation was
directionally slower and was not promoted.

Decision: rejected the locale-function AOT candidate. Canonical port 8806 is
unchanged; the next optimization must target a measured tight loop rather
than this large initialization/control-flow region.

## 2026-08-30 19:41 IDT: rejected focused NTDLL AOT

Tested an opt-in AOT table for the measured NTDLL loader hotspot at
`http://localhost:8901/?WASM_TPUT=1&WASM_BADIP=1&WASM_NTDLL_JIT=1`. The
candidate was built from the packaged Wine `ntdll.dll` and translated 468
blocks from `0x7bc12000-0x7bc14000`, with relocation-aware dispatch. The
dirty `vibe` tree remains at `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; all
unrelated dirty and untracked work is preserved. Candidate hashes were JS
`60edd0f248e055ecc88b86694d04e1fdd501dd710ae46211a0813ae92bfcd9ae`, WASM
`52ca323d05ad6052e56e182d35d10eff056aa43aeeb3c3bfab530ffa49a68530`, and
data `d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: the runtime logged `ntdll JIT 468 translated blocks loaded`, but
the worker then emitted `FATAL worker exception: memory access out of bounds`
before the first frame. The candidate canvas was therefore not a valid
rendering or FPS result.

Decision: removed the NTDLL AOT experiment and its build plumbing. Nothing
from this candidate was promoted; canonical port 8806 remains unchanged.

## 2026-08-30 19:25 IDT: canonical black-screen smoke check

Tested the explicit canonical URL
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1` using the preserved
interpreter-only control profile. The dirty `vibe` tree remains at
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`, with unrelated dirty and
untracked work preserved. Served artifact hashes are JS
`45106678b61cc5c173fadb742cec343dad8903eaeda3cdd5cd0f841b37452608`, WASM
`c81365d623730a8b725d40125ad98be6065ba61858b7a99c88c71313af5e8745`, and
data `d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: the 12-second check ended during startup with no first frame.
The extended check reached its first frame at 26.8s, then reported a real
changing 640x400 canvas (`non-black: 8`, 73 frames at the 25-second sample),
input ready (`keys 2`, `mouse 2`), and audio on at 22050Hz/2ch. No
`BADIP`, `FATAL`, `JITBAD`, `JITBADEIP`, or `RuntimeError` was observed.

Conclusion: the promoted artifact is not black-screened; the apparent black
screen is startup/precaching latency or an incorrect server root. Use the
explicit URL above, hard-refresh, and allow roughly 30 seconds for the first
frame before diagnosing a rendering regression.

## 2026-08-30 19:22 IDT: rejected optimized memset-loop hook; canonical remains usable

Retested the opt-in MSVCRT `memset` SSE-loop hook at
`http://localhost:8900/?WASM_TPUT=1&WASM_BADIP=1&WASM_MEMSET_LOOP=1` against
the same candidate artifact without `WASM_MEMSET_LOOP`. The dirty `vibe`
tree remains at `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; unrelated dirty and
untracked work is preserved. Candidate hashes were JS
`cfebe0c1bd90fb7f0a96be916304d2bf16582f456f1b4c2b6183d7cc47d2f8c9`, WASM
`7baededc35729a53c3daab1b93c48c0f66aaaaa46d456eefc29c331738002e4a`, and
data `d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: the enabled run logged `native msvcrt memset SSE loop @
3ee39b80` and reached changing 640x400 WebGL frames, input ready, and audio
on with no `BADIP`, `FATAL`, `JITBAD`, `JITBADEIP`, or `RuntimeError`. The
control became timing-stalled at `flips=3840` around guest `t=10.7` and was
therefore inconclusive as an FPS comparison; its canvas sample was not a
usable sustained-render result.

Decision: removed the opt-in hook and its 64-bit-store experiment. It was not
promoted. The canonical port 8806 artifact remains the verified
lookup-cache build; use the explicit canonical URL and hard-refresh the page.

## 2026-08-30 19:06 IDT: rejected relocation-aware memset loop hook

Tested an opt-in, skeleton-guarded hook for the MSVCRT `memset` SSE loop at
`http://localhost:8900/?WASM_TPUT=1&WASM_BADIP=1&WASM_MEMSET_LOOP=1`; the
same-artifact control omitted `WASM_MEMSET_LOOP`. The dirty `vibe` tree
remains at `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`, with unrelated dirty
and untracked work preserved. Candidate hashes were JS
`cfebe0c1bd90fb7f0a96be916304d2bf16582f456f1b4c2b6183d7cc47d2f8c9`, WASM
`8fce516b858df0f0f1cd396abc65280b5e8a6d02947ec09d495dfb8b0b79d921`, and
data `d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: the enabled run logged `native msvcrt memset SSE loop @
3ee39b80` and the control did not. Both reached real changing 640x400 WebGL
frames, input ready, audio on, and no `BADIP`, `FATAL`, `JITBAD`,
`JITBADEIP`, or `RuntimeError`. At comparable guest time, enabled reached
`flips=9644` at `t=23.2`, while control reached `9677` at `t=23.4`; the
enabled path was not a repeatable improvement.

Decision: rejected and removed the new hook registration and guard. The
canonical port 8806 artifact remains unchanged; the SSE loop stays
interpreter-side pending a larger optimization.

## 2026-08-30 18:52 IDT: rejected MSVCRT lookup cache

Tested a 256-entry positive/negative cache for MSVCRT AOT lookups at
`http://localhost:8899/?WASM_TPUT=1&WASM_BADIP=1`; the identical-artifact
control used `WASM_NO_GENCACHE=1`. The dirty `vibe` tree remains at
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`, with unrelated dirty and
untracked work preserved. Candidate hashes were JS
`d229f50aacde5f6bba5c29e83813fa6f4491625403d289234efcb2a68a75e6f7`, WASM
`1c926c3a82d60e6a1420428ba5cd616f34ebd1b32fa090ff84adc0900fbaace6`, and
data `d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: enabled and control both reached real changing 640x400 WebGL
frames, input ready, audio on, and no `BADIP`, `FATAL`, `JITBAD`,
`JITBADEIP`, or `RuntimeError`. Enabled reached `flips=9844` at guest
`t=23.9`; control reached `flips=10897` at `t=24.1`.

Decision: rejected and removed the MSVCRT lookup cache. The canonical port
8806 artifact remains the verified executable-lookup-cache build.

## 2026-08-30 18:40 IDT: rejected 4096-entry lookup cache

Tested a 4096-entry generated-block lookup cache at
`http://localhost:8898/?WASM_TPUT=1&WASM_BADIP=1`; the identical-artifact
control used `WASM_NO_GENCACHE=1`. The dirty `vibe` tree remains at
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`, with unrelated dirty and
untracked work preserved. Candidate hashes were JS
`6e939c8d49b96af5dbfebcbd23a7fce64400be03e7d4ad6c6738f92059d7318b`, WASM
`9a0756d6682cc3a95aecf4044859feea595812d16bf03d69d54a8a9a80365aec`, and
data `d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: both runs reached real changing 640x400 WebGL frames, input
ready, and audio on, with no `BADIP`, `FATAL`, `JITBAD`, `JITBADEIP`, or
`RuntimeError`. The enabled run reached `flips=6589` at guest `t=20.2` and
`9243` at `t=23.3`; the disabled control reached `6592` at `t=20.1` and
`10327` at `t=24.1`. The larger cache provides no repeatable improvement.

Decision: reverted to the verified 1024-entry cache; the canonical port 8806
artifact was not changed.

## 2026-08-30 18:27 IDT: rejected loop-only MSVCRT AOT

Tested a generated one-block translation of the MSVCRT `memset` SSE loop at
`http://localhost:8897/?WASM_TPUT=1&WASM_BADIP=1`; the same artifact's control
used `WASM_NO_MSVCRT_JIT=1`. The dirty `vibe` tree remains at
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`, with unrelated dirty and
untracked work preserved. Candidate hashes were JS
`2588a46538ebe78460b7d7b76988e0f72417567a8fa8922c6ca7a315b258c8e5`, WASM
`56ad93a3ceafedcdbf083493d51e1852b1ffa11335bd380c167ee65ece73119f`, and
data `d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: both A/B runs reached real changing 640x400 WebGL frames, input
ready, and audio on, with no `BADIP`, `FATAL`, `JITBAD`, `JITBADEIP`, or
`RuntimeError`. The loop-enabled run reached `flips=7808` at guest `t=25.6`;
the disabled control reached `flips=8074` at `t=24.7`. The enabled path was
directionally slower and was not promoted.

Decision: rejected loop-only AOT. The canonical lookup-cache artifact on
port 8806 is unchanged; retain the `WASM_NO_MSVCRT_JIT` switch for future
diagnostic builds.

## 2026-08-30 18:12 IDT: post-cache hotspot profile

Re-profiled the promoted artifact at
`http://localhost:8806/?WASM_TPUT=1&WASM_IPAGE=1&WASM_MODULES=1&WASM_BADIP=1`.
The canonical hashes remain JS
`45106678b61cc5c173fadb742cec343dad8903eaeda3cdd5cd0f841b37452608`, WASM
`c81365d623730a8b725d40125ad98be6065ba61858b7a99c88c71313af5e8745`, and
data `d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`;
the dirty `vibe` tree is still at `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`.

Observation: the diagnostic reached a real changing 640x400 canvas, input
ready, and audio on, with five non-black/four distinct captured frames and no
`BADIP`, `FATAL`, `JITBAD`, `JITBADEIP`, or `RuntimeError`. The leading misses
were again the MSVCRT SSE loop (`3ee39b80`–`3ee39b8c`, about 0.5M each), then
NTDLL synchronization code around `3f9239xx` (about 0.2M each); the earlier
`3f9284f5` cluster is no longer the top entry.

Hypothesis: the lookup cache removed enough dispatch overhead to expose the
MSVCRT/NTDLL instruction bodies as the next meaningful targets. The full
MSVCRT function AOT and tiny native wrappers were already rejected; continue
with a larger verified synchronization or SSE-region optimization.

## 2026-08-30 18:05 IDT: rejected full-function MSVCRT memset AOT

Tested full AOT translation of the MSVCRT `memset` function, including its
SSE loop, at
`http://localhost:8896/?WASM_TPUT=1&WASM_BADIP=1`. The dirty `vibe` tree
remains at `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`, with unrelated dirty
and untracked work preserved. The isolated candidate hashes were JS
`da06f7a21d4b899f7e7a64d462d6fffab4d358a24021df0630333d58c707ddac`, WASM
`9454be9b6f121293e6a93b7a17708d82affe1d568013f24761fd70241a38b025`, and
data `d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: the table loaded `msvcrt JIT 16 translated blocks`; the node
candidate ran stably at about 60 FPS for several minutes. The browser run
reached a real changing 640x400 canvas, reported input ready and audio on,
and emitted no `BADIP`, `FATAL`, `JITBAD`, `JITBADEIP`, or `RuntimeError`.
However, it reached about `flips=6486` by guest `t=26.6`, below the promoted
lookup-cache build's comparable `flips=8178` at `t=25.3`. This is a browser
performance regression despite functional correctness.

Decision: rejected the full-function MSVCRT AOT candidate; it was never
copied to port 8806. The promoted lookup-cache artifact remains canonical.

## 2026-08-30 16:55 IDT: promoted generated-block lookup cache

Added a 1024-entry direct-mapped cache in `gen_lookup_cached()` for both
positive and negative generated-block lookups. The dirty `vibe` tree remains
at `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; unrelated dirty and untracked
work was preserved. The promoted canonical artifact at
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1` has JS
`45106678b61cc5c173fadb742cec343dad8903eaeda3cdd5cd0f841b37452608`, WASM
`c81365d623730a8b725d40125ad98be6065ba61858b7a99c88c71313af5e8745`, and
data `d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: a same-artifact A/B at port 8895 reached real changing 640x400
WebGL 2 frames, reported `input: ready`, and emitted no `BADIP`, `FATAL`,
`JITBAD`, `JITBADEIP`, or `RuntimeError`. With the cache enabled, warm samples
reached `flips=9005` at guest `t=25.1`; with
`WASM_NO_GENCACHE=1`, the control reached `flips=8290` at `t=25.4`. The
canonical 20-second verification reached a real changing canvas, input ready,
audio on, and `FPSSAMPLE t=25.3 flips=8178`; its first frame was at 13.7s and
the harness recorded five distinct non-black frames. The shorter 12-second
smoke test ended before video initialization and is inconclusive.

Decision: promoted the cache build to port 8806 after the same-binary A/B;
the canonical server has COOP/COEP headers and serves the hashes above. Keep
the cache kill switch for regression control while the next optimization
targets the remaining dynamic-code execution path.

## 2026-08-30 16:34 IDT: rejected redundant SDL direct-entry seeds

Built an isolated generated-table candidate with explicit AOT roots for the
SDL atomic helpers and `SDL_GetVideoDevice`, served at
`http://localhost:8894/?WASM_TPUT=1&WASM_BADIP=1`. The dirty `vibe` tree is
still at `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`, with unrelated dirty and
untracked work preserved. Candidate hashes were JS
`6da17dbc93621b8a04a146287ffa5dc2ddc2a0254251ea8e45ef5c35d91080ed`, WASM
`c1ac9b49c44c7aa20d743af813332d18daffa485687ef9c74366a8e5af791b31`, and
data `d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: the build reported `translated 170737/170760 basic blocks`, the
same count as the control, and runtime logs showed no native SDL atomic or
video-device hook. The canonical URL remained the control. The candidate's
COOP/COEP run reached a real changing 640x400 WebGL 2 canvas, reported input
ready, and had no `BADIP`, `FATAL`, `JITBAD`, or `JITBADEIP`; warm samples
reached `FPSSAMPLE t=13.7 flips=5870`. Therefore the addresses were already
covered by the static table; the hotspot is dynamic code and these roots add
no useful work.

Decision: rejected the generated-entry experiment and restored the original
generated table. Continue targeting dynamic-code dispatch or a larger
interpreter region; do not promote this artifact.

## 2026-08-30 16:18 IDT: rejected RtlWakeAddressAll fast path

Tested an isolated NTDLL candidate at
`http://localhost:8893/?WASM_TPUT=1&WASM_BADIP=1`. The dirty `vibe` tree
remains at `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`, with unrelated dirty
and untracked work preserved. Candidate hashes were JS
`cf1c0f13b45c060840fe726f0258cf948dab0a454be6b87c6efbda09885bf278`, WASM
`8c0bec095a0aaac0c66834cffc784453708e77779a9c076198605a0f288701a6`, and
data `d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: the guarded hook logged `native RtlWakeAddressAll (empty bucket)`;
the run reached a real changing 640x400 WebGL canvas, accepted input, enabled
audio, and had no `BADIP`, `FATAL`, `JITBAD`, `JITBADEIP`, or `RuntimeError`.
Its final sample was about `flips=10163` at guest `t=24.2`, below the
same-profile no-hook result of about `flips=10678` at guest `t=24.0` (and the
first-frame cadence was also lower). These are directional measurements, not
a claim of a controlled FPS delta.

Decision: rejected and removed the wake-hook source changes; the canonical
rollback remains the control on port 8806. Continue with larger translated
regions or dispatch reduction rather than tiny native wrappers.

## 2026-08-30 15:56 IDT: rejected SDL atomic-helper hooks

Profiled the canonical rollback at
`http://localhost:8806/?WASM_TPUT=1&WASM_IPAGE=1&WASM_MODULES=1&WASM_BADIP=1`.
The dominant executable misses were `SDL_AtomicGet` and sibling helpers at
`0x0073ca**`; the canonical artifact was not changed. The dirty `vibe` tree
remains at `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`, with dirty and
untracked work preserved.

Observation: an isolated candidate served at
`http://localhost:8892/?WASM_TPUT=1&WASM_BADIP=1` registered all seven guarded
helpers (`CAS`, `Set`, `SetPtr`, `Add`, `Get`, `GetPtr`, `TryLock`) and reached a
real changing 640x400 WebGL canvas with input and audio; candidate hashes were
JS `5902837d53fca5efa69a46d542f7ed0c3424af1ed9b436864a80aec8ad935f37`, WASM
`5b48609bd9dbe8d8bdaa1727eb6921499d71522340b12be54400ea6ef80dbafc`, and data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.
The same candidate with `WASM_NO_ATOMICS=1` produced 11,641 flips by guest
`t=24.6`, versus 10,678 by `t=24.0` with the hooks; this is not a controlled
FPS improvement and is directionally slower. No `BADIP`, `FATAL`, `JITBAD`, or
`JITBADEIP` appeared.

Decision: rejected the atomic-helper candidate and removed its source changes;
the canonical rollback remains the control. The next target is the larger
NTDLL/loader cluster, not these tiny SDL functions.

## 2026-08-30 15:43 IDT: rolled back black-screen regression

The user reported that the promoted browser build was stuck on a black
screen. The canonical server remains
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the dirty `vibe` tree is
still at `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`, and dirty/untracked work
was preserved.

Observation: the promoted mid-function MSVCRT `memset` SSE-loop hook was
removed from the build and the replacement artifact was verified first at
`http://localhost:8890/?WASM_TPUT=1&WASM_BADIP=1`, then promoted to port 8806.
The replacement hashes are JS
`cd0045d92869c8d91137d1615ac28ee811ea8317f243447fe33348ea2e09dd43`, WASM
`8221a505b99479cd039337c0f0478218c8aa18cbb156745074cc8a300510e11e` and data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.
The clean 20-second verification reached a real 640x400 canvas, first frame
at 7.1s, input target `0x20028`, WebGL 2/OpenGL 3.3, and sustained changing
frames (`flips=13752` at guest `t=22.0`); no `BADIP`, `FATAL`, `JITBAD`, or
`JITBADEIP` appeared. The hook log is absent; ordinary native `memset`
remains active.

Hypothesis: the mid-function hook caused the user's black screen despite the
earlier automated candidate run; it is disabled pending a smaller, validated
implementation. The canonical URL now serves the rollback bundle.

## 2026-08-30 15:31 IDT: profiled promoted memset-loop build

Profiled the promoted bundle at
`http://localhost:8806/?WASM_TPUT=1&WASM_IPAGE=1&WASM_MODULES=1&WASM_BADIP=1`.
Hashes were JS
`0bb2fc9610975b8d0995da9ba4ef0faea64265f6d39061f3b2eb4a3c477f42dd`, WASM
`3f7030bded23a8e63ae10ca169e53e944de705fcefb97c5b7b238b4dcb60f8fe`, and
data `d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.
The dirty `vibe` tree remains at `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`;
dirty and untracked work was preserved.

Observation: the run logged `native msvcrt memset SSE loop @ 3ee39b80` and
reached the real WebGL canvas/context without `BADIP`, `FATAL`, `JITBAD`, or
`JITBADEIP`. Warm guest samples reached `FPSSAMPLE t=16.0 flips=7582`
before the diagnostic run's timing stalls. The final `IMISS top` contained
new dynamic-code entries (`0073cacx`, `0070d7a0`) and NTDLL entries
(`3f9284xx`), but also retained `3ee39b80/83/86/8a/8c` at 0.5M each.

Hypothesis: the mid-function hook is active and materially improves the clean
render path, but the IPAGE counters include an unresolved residual entry path
or pre-hook execution into the same SSE bytes. Do not claim complete miss
elimination; map the call/entry source for the residual addresses before
altering the working hook.

## 2026-08-30 15:30 IDT: promoted native MSVCRT memset SSE loop

Added a skeleton-guarded mid-function native hook for MSVCRT `memset`'s
internal 32-byte `movaps` loop at runtime `3ee39b80` (static
`0x10049b80`). It writes the exact repeated four-word pattern from EAX/EDX,
preserves ESI/EBX loop state, and resumes at the verified epilogue. The
candidate was served with COOP/COEP at
`http://localhost:8891/?WASM_TPUT=1&WASM_BADIP=1` and promoted to the
canonical URL `http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`. The promoted
hashes are JS
`0bb2fc9610975b8d0995da9ba4ef0faea64265f6d39061f3b2eb4a3c477f42dd`, WASM
`3f7030bded23a8e63ae10ca169e53e944de705fcefb97c5b7b238b4dcb60f8fe`, and
data `d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.
The dirty `vibe` tree remains at `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`;
dirty and untracked work was preserved.

Observation: the candidate logged `native msvcrt memset SSE loop @ 3ee39b80`,
reached a real changing 640x400 canvas and WebGL 2/OpenGL 3.3 context,
accepted input (`wasm_input: input target window 0x20028`), enabled SDL audio,
and completed the 22-second run without `BADIP`, `FATAL`, `JITBAD`, or
`JITBADEIP`. The final guest sample was
`FPSSAMPLE t=22.2 flips=13807 fps=1361.6`, with active mapper counters
(`vl=68/13552`, `mv=75/3367`, `mv1=2/113`). The earlier corrected-hook
candidate reached 13,671 flips at 26.3s; both runs show the same sustained
high-cadence behavior after startup.

Hypothesis: intercepting the internal loop removes the dominant MSVCRT SSE
boundary and produces a material FPS improvement while retaining the export
function's original epilogue contract. Re-profile the canonical bundle to
confirm the `3ee39b8x` cluster is gone, then target the next remaining
MSVCRT/NTDLL helper rather than expanding this hook further.

## 2026-08-30 01:53 IDT: confirmed UCRT wcschr cluster removal

Profiled the promoted bundle at
`http://localhost:8806/?WASM_TPUT=1&WASM_IPAGE=1&WASM_MODULES=1&WASM_BADIP=1`.
Hashes were JS
`3b77402b6655bdac7bf8942e4e3c0ed6b29b5fd1572017e4884580fb203123fe`, WASM
`cfbfcdeecfb21b46ebef18fb238c29227d000bdfcea90f728874836230a76265`, and
data `b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`.
The dirty `vibe` tree remains at `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`;
dirty and untracked work was preserved.

Observation: the run logged the native UCRT hook and reached the real canvas
and OpenGL setup without `BADIP`, `FATAL`, `JITBAD`, or `JITBADEIP`. The prior
UCRT `3ea91e3x` cluster no longer appeared in `IMISS top`. The new dominant
list is MSVCRT `3ee39b80/83/86/8a/8c` at 0.5M each, followed by a separate
MSVCRT `3ee3b5xx` cluster at 0.3M each.

Hypothesis: `wcschr` interception is effective; continue by mapping
`3ee3b5xx` to its exact MSVCRT export/body and keep the unresolved internal
`memset` SSE loop separate from that investigation.

## 2026-08-30 01:52 IDT: promoted native UCRT wcschr hook

Added a bounded native UCRT `wcschr` implementation for the verified UTF-16
scan loop at `0x10091e20`, with a corrected prologue skeleton guard. The
successful candidate was served with COOP/COEP at
`http://localhost:8893/?WASM_TPUT=1&WASM_BADIP=1` and promoted to the
canonical URL `http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`. The candidate
and now canonical hashes are JS
`3b77402b6655bdac7bf8942e4e3c0ed6b29b5fd1572017e4884580fb203123fe`, WASM
`cfbfcdeecfb21b46ebef18fb238c29227d000bdfcea90f728874836230a76265`, and
data `b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`. The
dirty `vibe` tree remains at `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`;
dirty and untracked work was preserved.

Observation: the successful run logged `native ucrtbase wcschr @ 3ea91e20`,
reached a real changing 640x400 canvas and OpenGL context, accepted input
(`keys 2, mouse 2`), enabled audio, and completed without `BADIP`, `FATAL`,
`JITBAD`, or `JITBADEIP`. Its final guest sample was
`FPSSAMPLE t=25.9 flips=3645 fps=362.3`, with active mapper counters
(`vl=138/55206`, `mv=150/12971`, `mv1=3/307`). The preceding first candidate
run at port 8894 was not promoted because its guard correctly reported
`ucrtbase wcschr skeleton differs at 3ea91e20`; it otherwise remained on the
canonical behavior.

Hypothesis: the corrected hook removes the UCRT UTF-16 search boundary while
preserving the null-terminator and character-match semantics. Re-profile the
canonical bundle next to confirm the `3ea91e3x` cluster is gone and select
the next remaining measured helper.

## 2026-08-30 01:16 IDT: verified TlsGetValue hotspot removal

Re-profiled the promoted bundle at
`http://localhost:8806/?WASM_TPUT=1&WASM_IPAGE=1&WASM_MODULES=1&WASM_BADIP=1`.
Hashes were JS
`946e4b7371447910d53dd21cd9a9f2025acc915a40aace61be45685da7dc9869`, WASM
`a166f31a2ffeac1680bede02a222f7dfc8f1b0ed13c02397cffc691e419c89bc`, and
data `b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`.
The dirty `vibe` tree remains at `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`;
dirty and untracked work was preserved.

Observation: the run logged both native TLS hooks and a real 640x400 canvas
with OpenGL initialization. The former kernel32/kernelbase TLS miss cluster
(`3f78f5f0` and `3f247dfx`) no longer appeared in `IMISS top`; the remaining
top entries were MSVCRT `3ee39b80/83/86/8a/8c` at 0.5M each, followed by
`3ea91e3x` and `3ee3b5xx`. No `BADIP`, `FATAL`, `JITBAD`, or `JITBADEIP` was
logged. This was a diagnostic profile, so its zero overlay FPS is not used
as a gameplay cadence measurement.

Hypothesis: TlsGetValue interception is effective and removes the targeted
kernel32/kernelbase interpreter work. Continue with the still-unresolved
MSVCRT internal memset path and newly exposed `3ea91e3x`/`3ee3b5xx` clusters,
using static address mapping before attempting another replacement.

## 2026-08-30 01:15 IDT: promoted native TlsGetValue hooks

Added bounds-checked native implementations for the kernel32
`TlsGetValue` thunk and kernelbase implementation, including the 64-slot TEB
array, expansion slots, `LastErrorValue`, and stdcall stack cleanup. The
candidate was served with COOP/COEP at
`http://localhost:8895/?WASM_TPUT=1&WASM_BADIP=1` and promoted to the
canonical URL `http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`. The candidate
and now canonical hashes are JS
`946e4b7371447910d53dd21cd9a9f2025acc915a40aace61be45685da7dc9869`, WASM
`a166f31a2ffeac1680bede02a222f7dfc8f1b0ed13c02397cffc691e419c89bc`, and
data `b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`. The
dirty `vibe` tree remains at `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`;
dirty and untracked work was preserved.

Observation: the run logged `native TlsGetValue @ 3f78f5f0` and
`native kernelbase TlsGetValue @ 3f247db0`, reached a real changing 640x400
canvas and OpenGL context, accepted input (`keys 2, mouse 2`), enabled audio,
and completed the 22-second test without `BADIP`, `FATAL`, `JITBAD`, or
`JITBADEIP`. The final guest sample was `FPSSAMPLE t=25.9 flips=3194 fps=346.2`
with active mapper counters (`vl=144/57762`, `mv=157/13558`,
`mv1=3/321`). The CDP overlay aggregate FPS field remained zero in this run;
guest samples and the changing canvas are the decisive measurements.

Hypothesis: the kernel32/kernelbase TLS boundary is now removed from the
interpreter while preserving valid and invalid index behavior. Keep this as
the canonical control and re-profile the remaining ntdll relocation and
MSVCRT/SSE clusters before selecting another hook.

## 2026-08-30 00:56 IDT: rejected MSVCRT memset SSE translation

The 55-block candidate added the verified `___udivmoddi4` range and the
MSVCRT `memset` range `0x10049ae0-0x10049c00`, including its `movaps` loop.
It was served with COOP/COEP at
`http://localhost:8896/?WASM_TPUT=1&WASM_IPAGE=1&WASM_MODULES=1&WASM_BADIP=1`.
Candidate hashes were JS
`82d7831e72eb07ba67d51340fcd28271c7df36492da72eebb7c0f109a945f4c0`, WASM
`cf59b795e66f9091e177bd55a0bf27b33af35b47bb3cfd6f1c5da2766b5259d6`, and
data `b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`.
The canonical bundle was not changed; the dirty `vibe` tree remains at
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`, with dirty and untracked work
preserved. The generated source table was restored to the canonical 39-block
version.

Observation: the candidate logged `msvcrt JIT 55 translated blocks loaded`
and reached the real 640x400 canvas and OpenGL initialization, but gameplay
stalled at 188 guest flips (`FPSSAMPLE t=20.6`) with `fps: 0.0` in the CDP
summary. The `IMISS top` list still contained
`3ee39b80/83/86/8a/8c=0.5M` each. No `BADIP`, `FATAL`, `JITBAD`, or
`JITBADEIP` appeared, but the stall and unchanged hotspot make this an
invalid performance candidate.

Hypothesis: the hot execution at `3ee39b80` is reached through an internal
or indirect path that the export-range AOT table does not safely intercept;
the translated range also changes control flow enough to stall the game.
Keep it rejected and investigate the call/entry path before attempting any
memset replacement again. The next independent measured cluster is the
kernel32 `3f78f5f0` path, with NTDLL relocation code (`3f9239xx`) tracked
separately.

## 2026-08-30 00:41 IDT: profiled remaining MSVCRT SSE loop

The promoted case-fold bundle was profiled at
`http://localhost:8806/?WASM_TPUT=1&WASM_IPAGE=1&WASM_MODULES=1&WASM_BADIP=1`.
The tested canonical hashes were JS
`473e5764806d088ec1b06e11648b449b833f654e559a9780e9d642bb4d4b00d8`, WASM
`a47e0e32dc8f456cc15bc9b664597208ded4a632a1fdf25983afd58fb0482e10`, and
data `b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`.
The dirty `vibe` tree remains at `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`;
dirty and untracked work was preserved.

Observation: the run reached a real 640x400 changing canvas and logged the
expected OpenGL context and the native case-fold/byte-copy hooks. Its decisive
`IMISS top` entry remained `3ee39b80`, `3ee39b83`, `3ee39b86`, `3ee39b8a`,
and `3ee39b8c`, each about 0.5M misses. Module-base mapping identifies this
as MSVCRT `memset`'s `movaps` loop at static `0x10049b80`, while the export
entry is `0x10049ae0`. No `BADIP`, `FATAL`, `JITBAD`, or `JITBADEIP` was
logged during the diagnostic.

Hypothesis: the existing native memset export hook is not reached for every
call path; the remaining hot path is the internal SSE loop itself. The next
candidate will translate the verified `0x10049ae0-0x10049c00` memset range
alongside the existing 39-block table, using the translator's SSE support and
its single verified fall-through skeleton.

## 2026-08-30 00:38 IDT: promoted native ASCII case-fold hooks

Added guarded native `tolower` and `toupper` hooks for the hot ASCII path in
the fixed Wine MSVCRT image. The candidate was served with COOP/COEP at
`http://localhost:8897/?WASM_TPUT=1&WASM_BADIP=1` and promoted to the
canonical URL
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`. The candidate and now
canonical hashes are JS
`473e5764806d088ec1b06e11648b449b833f654e559a9780e9d642bb4d4b00d8`, WASM
`a47e0e32dc8f456cc15bc9b664597208ded4a632a1fdf25983afd58fb0482e10`, and
data `b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`. The
dirty `vibe` tree remains at `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`;
dirty and untracked work was preserved.

Observation: the candidate logged `msvcrt JIT 39 translated blocks loaded`,
`native tolower @ 3ee4ff40`, and `native toupper @ 3ee4ff80`. It reached a
real changing 640x400 canvas, accepted input (`keys 2, mouse 2`), enabled
audio, and ran through the 25-second test without `BADIP`, `FATAL`,
`JITBAD`, or `JITBADEIP`. Guest samples reached 5,684 flips at 29.1s, with
the normal mapper counters active (`vl=135/54127`, `mv=312/14946`, `mv1=66/2272`)
in the final sample. The CDP overlay's aggregate FPS field remained zero in
this run despite the changing canvas; the guest `FPSSAMPLE` counters are the
decisive cadence measurement here.

Hypothesis: the guarded hooks remove the locale-independent ASCII work from
the remaining `3ee4ffxx` cluster while preserving the slow locale path when
the CRT locale pointer is uninitialized. The next measured hotspot remains
the MSVCRT SSE/memset cluster at `3ee39b80` and then the kernel32 cluster at
`3f78f5f0`.

## 2026-08-30 00:22 IDT: promoted NTDLL byte-copy hooks

Added native hooks for NTDLL `memcpy`, `memmove`, and `memset`, reusing the
existing bounds-checked host implementation used by MSVCRT. The candidate was
tested at
`http://localhost:8898/?WASM_TPUT=1&WASM_BADIP=1` and promoted to the canonical
URL `http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`. The candidate and now
canonical hashes are JS
`b727a3e0067b845e5fa010f4f6bf49f81036b186f66dc3b1639094425fc6731c`, WASM
`90720a2e136625f5c7f46c3ffdbbb4f3efa2ed53329ac4296e541e116a35a7ee`, and data
`b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`. The
dirty `vibe` tree remains at `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`;
dirty and untracked work was preserved.

Observation: the candidate logged `native ntdll memcpy @ 3f9717c0`,
`native ntdll memmove @ 3f9718b0`, and `native ntdll memset @ 3f971920`.
At 20 seconds it had a real changing 640x400 canvas, input ready (`keys 2,
mouse 2`), audio on, and no `BADIP`, `FATAL`, `JITBAD`, or `JITBADEIP`. It
reported `fps: 40.4`, 520 presented frames, first frame 14.3s; the guest
counter reached 3,195 flips at 20.8s. The same-time canonical control reported
`fps: 45.1`, first frame 15.6s, and 2,466 flips at 20.0s. The NTDLL
`3f9717xx` entries disappeared from the candidate's later `IMISS top` list.

Hypothesis: NTDLL copy traffic was a material interpreted-load source and the
hooks improve startup-to-render throughput without changing guest-visible copy
semantics. Keep the promotion as the new control and next profile the remaining
MSVCRT SSE cluster (`3ee39b80–3ee39b8c`) and the `3ee4ffxx` cluster separately.

## 2026-08-30 00:03 IDT: rejected focused MSVCRT memset/SSE candidate

The focused table was expanded to retain the verified `___udivmoddi4` range
and translate `memset` (`0x10049ae0-0x10049c00`), including its `movaps` loop.
The candidate was served at
`http://localhost:8810/?WASM_TPUT=1&WASM_BADIP=1` (plain `http.server`, so
COOP/COEP was absent) with JS
`8d0c558fdbc31493bdec59759893b74bda097cfcc72f694f52fb707709c65ab6`, WASM
`b51516fb9328855e627d834b5a2c42aea224b9e7f27005ac609c4392559b91b5`, and data
`b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`. The
dirty `vibe` tree remained at `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`;
dirty and untracked work was preserved.

Observation: the build completed and produced a changing 640x400 canvas, but
the test had input disabled because the server lacked COOP/COEP. It reached
`fps: 11.0`, 231 frames at 20 seconds, then oscillated between roughly 16 and
950 FPS while the game was intermittently stalled. The log showed no
`JITBAD`, `JITBADEIP`, or fatal error, but did not show the expected `msvcrt
JIT` line, so this is not a valid performance promotion and was rejected.

Hypothesis: the focused browser build did not arm its CRT table in this run,
and the plain server invalidated the input/performance comparison. Restore the
39-block control table and rerun any SSE candidate only through the canonical
COOP/COEP server before drawing conclusions.

## 2026-08-29 23:42 IDT: promoted focused MSVCRT AOT into canonical build

Separated the focused `___udivmoddi4` generated table from the existing full
MSVCRT research artifact as `msvcrt_focused_gen_blocks.c`. `build-node.sh` now
selects that file and defines `WEBWINE_MSVCRT_FOCUSED_AOT` only when the
explicit `MSVCRT_AOT_RANGE` is supplied; the focused runtime enables its 39
blocks automatically, while the ordinary/full-DLL paths retain their prior
opt-in behavior. The canonical bundle at
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1` now contains the focused
build. The dirty `vibe` tree remains at
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; dirty and untracked work was
preserved. Canonical hashes are JS
`8e15425c4682de9ae6b933407291ea1c1dd6a418e62456ce838e89aeefa745a9`, WASM
`e324c58e6ddf06d27982da7186c9c5a1503c8d76da05504f811b6fda49eb4257`, and data
`b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`.

Observation: the no-query-flag candidate logged `msvcrt JIT 39 translated
blocks loaded`, reached a changing 640x400 canvas, accepted input (`keys 2,
mouse 2`), enabled audio, and completed 25 seconds with 692 frames (`fps:
62.6`). It logged no `BADIP`, `FATAL`, `JITBAD`, or `JITBADEIP`. The original
full `msvcrt_gen_blocks.c` artifact was preserved separately.

Hypothesis: the focused table safely removes the measured internal divide-loop
load in the normal packaged build without enabling the unverified full-DLL AOT
surface. Profile the new canonical bundle next for the remaining ntdll and
CRT clusters.

## 2026-08-29 23:32 IDT: isolated __udivmoddi4 AOT runtime verified

The recovered direct-link bundle containing only the 39-block
`___udivmoddi4` AOT range was tested at
`http://localhost:8858/index.html?WASM_MSVCRT_JIT=1&WASM_TPUT=1&WASM_IPAGE=1&WASM_BADIP=1`.
It was built from dirty `vibe` at
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; dirty and untracked work was
preserved. Candidate hashes are JS
`3ae982db4d04d7b93750d3023ef6ea27ddc3f36e23b8a29b24fdb7f7a47b34f3`, WASM
`55b182518445c82a1eec6f8288e4f547dcf0344f6234eeb381fca657bcf5522f2`, and data
`b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`.

Observation: the runtime logged `msvcrt JIT 39 translated blocks loaded`; the
former `0x3ee002xx` divide-loop entries disappeared from `IMISS top`. The
28-second run reached a changing 640x400 canvas, accepted input (`keys 2,
mouse 2`), enabled audio, produced 678 frames (`fps:49.3`), and logged no
`BADIP`, `FATAL`, `JITBAD`, or `JITBADEIP`. The focused candidate remains
opt-in and was not copied over the canonical default bundle; port 8806 is
unchanged.

Hypothesis: interpreter-compatible AOT translation, rather than the failed
manual native ABI shortcut, safely removes this hot long-division loop. The
`build-node.sh` now accepts explicit `MSVCRT_AOT_RANGE=0x100100f0-0x10010459`
to regenerate this focused table reproducibly; the runtime remains opt-in via
`MSVCRT_AOT=1` plus `WASM_MSVCRT_JIT=1`. The next task is proving that startup
path under the packaged default before considering promotion.

## 2026-08-29 23:28 IDT: isolated __udivmoddi4 AOT link failure

To preserve the guest stack contract after the failed native divide shortcut,
the MSVCRT AOT table was narrowed to the `___udivmoddi4` range
`0x100100f0-0x10010459` (39 translated blocks). The browser build was attempted
from dirty `vibe` at `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; no candidate
artifact hashes exist because linking/post-processing did not complete. The
full generated MSVCRT artifact was restored afterward.

Observation: compilation completed, but the link pipeline failed before a
usable JS bundle was produced; `webwine-bw.wasm` remained zero bytes. No
browser test was run and no bundle was promoted. The canonical port 8806
artifacts remain JS
`a7c55e92f541579d252dc66373e28ca010de66a5211a626622443006fffcc70f`, WASM
`d2223e9e1b19f9482b3b8177375d9799baca96fc33b1c5fb60611571ab544f66`, and data
`b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`.

Hypothesis: the current MSVCRT AOT integration requires a complete generated
table or has unresolved cross-range link assumptions; a narrow range cannot be
used through the existing switch without fixing that build architecture.

## 2026-08-29 23:21 IDT: canonical regression after MSVCRT stub promotion

Retested the promoted bundle at the exact URL
`http://localhost:8806/?WASM_TPUT=1&WASM_IPAGE=1&WASM_BADIP=1`. The dirty
`vibe` tree remains at `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; dirty and
untracked work was preserved. Artifact hashes were JS
`a7c55e92f541579d252dc66373e28ca010de66a5211a626622443006fffcc70f`, WASM
`d2223e9e1b19f9482b3b8177375d9799baca96fc33b1c5fb60611571ab544f66`, and data
`b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`.

Observation: the run logged `native _adj_fptan (stub) @ 3ee012b0`, reached a
changing 640x400 canvas, accepted input (`keys 2, mouse 2`), enabled audio,
and produced 475 frames in 20 seconds (`fps:43.3`) with no `BADIP`, `FATAL`,
`JITBAD`, or `JITBADEIP`. The remaining `IMISS top` was the mapped internal
`0x3ee002xx` divide-helper cluster.

Hypothesis: the promoted stub shortcut is regression-clean; the divide helper
requires a call-site/argument trace before another optimization attempt.

## 2026-08-29 23:18 IDT: promoted MSVCRT _adj_fptan stub shortcut

The corrected MSVCRT mapping identified `_adj_fptan` at runtime `0x3ee012b0`.
Wine implements this export as a trace-only Pentium workaround stub, so a
guarded `NAT_MIDEBUG` return was added. The verified bundle is served at
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`, from dirty `vibe` at
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; dirty and untracked work was
preserved. Promoted hashes are JS
`a7c55e92f541579d252dc66373e28ca010de66a5211a626622443006fffcc70f`, WASM
`d2223e9e1b19f9482b3b8177375d9799baca96fc33b1c5fb60611571ab544f66`, and data
`b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`.

Observation: the candidate logged `native _adj_fptan (stub) @ 3ee012b0`,
reached a changing 640x400 canvas, accepted input (`keys 2, mouse 2`),
enabled audio, and completed 16 seconds with 81 frames (`fps:40.5`) and no
`BADIP`, `FATAL`, `JITBAD`, or `JITBADEIP`.

Hypothesis: this removes only trace-stub interpreter work and cannot affect
x87 state or guest-visible data. Keep it canonical and continue mapping the
remaining `0x3ee002xx` helper cluster using the corrected MSVCRT base.

## 2026-08-29 23:15 IDT: rejected __udivmoddi4 native shortcut

The `0x3ee002xx` cluster was mapped to the internal MSVCRT/libgcc
`___udivmoddi4` long-division helper at `0x3ee000f0`. A guarded host-`uint64_t`
implementation was tested at
`http://localhost:8857/index.html?WASM_TPUT=1&WASM_IPAGE=1&WASM_BADIP=1`, from
dirty `vibe` at `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; candidate hashes were
JS `b11cb82df5e1b8644f64f7dd0b91cb4a735e1c9a01d67004654d35ad9d59ece0`, WASM
`0db0e1f2910d714f0d4b283eaab8e8120dd73ad7f4d300d93a0096ed041659a4`, and data
`b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`.

Observation: the candidate logged `native __udivmoddi4 @ 3ee000f0` but failed
during startup with `FATAL worker exception: memory access out of bounds`
before a frame. The hook was removed and was not promoted.

Hypothesis: the internal helper's actual call/stack contract differs from the
standard standalone libgcc ABI in this runtime. Do not retry without tracing a
known call site and validating arguments and stack ownership.

## 2026-08-29 23:05 IDT: rejected isolated MSVCRT helper AOT

The whole-DLL MSVCRT AOT failure was narrowed to a 37-block table covering
only `_mbsbtype` and `_mbsbtype_l` (`0x100202b0-0x10020459`), with external
locale and errno calls left to the interpreter. The candidate was tested with
`WASM_MSVCRT_JIT=1` at
`http://localhost:8855/index.html?WASM_MSVCRT_JIT=1&WASM_TPUT=1&WASM_IPAGE=1&WASM_BADIP=1`,
from dirty `vibe` at `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; hashes were JS
`f74d23fe71404bf3822f3a743a617c023e04225a573b1dcc5a03f67ecce7caf3`, WASM
`6a44a9336cd9c5e9dc552b0e937f59a1352b162d81a139c8f76651df9e921cd2`, and data
`b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`.

Observation: the runtime logged `msvcrt JIT 37 translated blocks loaded`,
reached a changing 640x400 canvas with input (`keys 2, mouse 2`) and audio
active, and produced no `BADIP`, `FATAL`, `JITBAD`, or `JITBADEIP`. However,
the final profile still reported the same `0x3ee002xx` miss cluster and only
125 frames after a late first frame (`fps:37.6`); the candidate was not
promoted. The full generated MSVCRT artifact was restored, and the canonical
port 8806 bundle remains unchanged.

Hypothesis: the `0x3ee002xx` addresses are not `_mbsbtype` despite their
numeric proximity under the earlier base assumption; the isolated table did
not cover the actual hot entry. Re-map the runtime module base and exact PE
RVA before attempting another MSVCRT or x87 optimization.

## 2026-08-29 22:57 IDT: rejected whole-msvcrt AOT experiment

The remaining exact miss cluster was mapped to the `msvcrt!_mbsbtype` /
`_mbsbtype_l` region at `0x3ee002xx`. The repository's opt-in generated
MSVCRT table was tested with `WASM_MSVCRT_JIT=1` at
`http://localhost:8854/index.html?WASM_MSVCRT_JIT=1&WASM_TPUT=1&WASM_IPAGE=1&WASM_BADIP=1`.
The candidate was built from the dirty `vibe` tree at
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; hashes were JS
`01ce7fc268f7496a350097a27fb1339bdfeefde4d7d42301cef5bc894ab9b9e0`, WASM
`0d7a575ca802010ae9f338d6222ffc429cd96ef942c59b54dc39fc06c3cc746b`, and data
`b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`.

Observation: the runtime logged `msvcrt JIT 21145 translated blocks loaded`,
accepted input (`keys 2, mouse 2`), but ended with
`FATAL worker exception: memory access out of bounds` before producing a frame.
The candidate was not promoted; the canonical port 8806 bundle is unchanged.

Hypothesis: the existing whole-DLL MSVCRT AOT table has an unverified startup
ABI or memory-model path, so it cannot be used to optimize the locale helper as
a DLL-wide switch. Any follow-up must isolate `_mbsbtype`/`_mbsbtype_l` or prove
the first failing translated entry before enabling it.

## 2026-08-29 22:51 IDT: promoted AOT mvlineasm4 epilogue

The mapper-tail investigation showed that the fixed `mvlineasm4` epilogue was
excluded only because the entire self-modifying window was excluded from AOT.
Added a separate translator range, `0x632630-0x63265b`, while keeping the
self-modifying body excluded. This emits the normal interpreter-compatible
blocks for `0x632630` and `0x63263c`, so stack handling remains in the existing
guest model. The promoted bundle is served at
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`. The dirty `vibe` tree is at
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; dirty and untracked work was
preserved. Promoted hashes are JS
`a03e5bb61fd4c99676c2e5d8fa7eaec8d1a37fab64fc41e7682073fb29a09ede`, WASM
`410bd8df4eda7f320524599b33ca1749b2f151d9964a82fe5bf12bc841eb9e2a`, and data
`b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`.

Observation: the candidate logged `wasm_x86: JIT 170727 translated blocks
loaded`, reached a changing 640x400 canvas, accepted input (`keys 2, mouse 2`),
enabled audio, and completed both a 22-second gameplay run (`fps:65.8`, 666
frames) and an 18-second miss-profile run (`fps:51.0`, 545 frames) without
`BADIP`, `FATAL`, `JITBAD`, or `JITBADEIP`. The profile's `IMISS top` moved to
the msvcrt `0x3ee002xx` startup/locale cluster; the former `0x6326xx` mapper
epilogue cluster no longer appeared.

Hypothesis: this removes a recurring native-mapper-to-interpreter boundary and
should reduce interpreter overhead without changing mapper arithmetic or stack
ownership. Keep the new bundle as canonical and profile the remaining msvcrt
and ntdll clusters next.

## 2026-08-29 22:41 IDT: rejected mvlineasm4 epilogue fold

The post-timing-thunk profile showed the fixed `mvlineasm4` epilogue at
`0x632630–0x63265a` among the remaining misses. A candidate native tail was
implemented with a skeleton guard, committing the four persistent accumulators,
restoring the six saved registers, and returning after the known final
`dec 1 -> 0`. The candidate was built from the dirty `vibe` tree at
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b` and tested at
`http://localhost:8852/index.html?WASM_TPUT=1&WASM_BADIP=1`; candidate hashes
were JS `b4f90ab754169cd63a1d74a32bde89b7d4ca2387b4ac81c5c3628202fb0f7e3e`,
WASM `79bb51900ce0cc8de2bde1b69a0a9edb0d45a53e59294295bf3645bcacca15c4`, and
data `b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`.

Observation: the candidate logged `native mvlineasm4 epilogue @ 00632630`,
reached a changing 640x400 canvas with `frames: 360`, accepted input (`keys 2,
mouse 2`) and enabled audio, but then logged
`BADIP transition prev=00632630 eip=0032fad8 ... ret=04e4024e` followed by
`FATAL worker exception: memory access out of bounds`. The candidate was not
promoted; the canonical bundle at port 8806 is unchanged. The failed candidate
source was removed, and `git diff --check` passes.

Hypothesis: `0x632630` is reached through more than one stack shape; the native
mapper's re-entry contract is not sufficient to infer the six saved registers
and return address at this boundary. Do not retry this tail without tracing the
entry path and proving the stack layout for every caller.

## 2026-08-29 22:28 IDT: promoted kernel32 performance thunks

The post-`GetCurrentThreadId` profile identified `0x3f78e72c` and its
neighboring bytes as kernel32's `QueryPerformanceCounter` and
`QueryPerformanceFrequency` import thunks. Added guarded native registrations
for both, reusing the verified ntdll counter/frequency implementations. The
native hook table and its GL metadata arrays were widened together from 512 to
2048 slots; the first candidate exposed a stale 512-entry GL array and was
rejected before promotion. The corrected promoted bundle is served at
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`. Hashes are JS
`2a3486be27643e46e4112e11c95fbe39674e4513db28b694a5b59ebcc097413f`, WASM
`bee559d82e5324e0d9e78a2feef4d84ac569ae85e94d5c5268d559d59e183f8e`, and data
`b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`. The tree
remains dirty on `vibe` at `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; dirty
and untracked work was preserved.

Observation: the corrected 22-second browser run logged native hooks at
`3f941820`, `3f9417e0`, `3f79cc50`, `3f78e72c`, and `3f78e740`; it reached a
changing 640x400 canvas, accepted input (`keys 2, mouse 2`), enabled audio,
and had no `FATAL`, `BADIP`, `JITBAD`, or `JITBADEIP`. Warm intervals reported
851–1,144 FPS. The `0x3f78e72c` thunk cluster disappeared from `IMISS top`; the
remaining top pages were ntdll (`0x3f94xxxx`/`0x3f92xxxx`) and CRT, with
`0x0073xxxx` startup/dynamic code also visible.

Hypothesis: bypassing both kernel32 timing thunks removes another repeated
interpreter boundary and the larger collision-free table lets the hooks stay
armed. Keep this as the new canonical control and target the remaining ntdll
helper cluster next.

## 2026-08-29 22:13 IDT: promoted GetCurrentThreadId hook

The post-counter profile's largest exact miss cluster (`0x3f79cc50`, with
repeated misses at `+6` and `+9`) maps to the kernel32 export
`GetCurrentThreadId`. Added an exact skeleton-guarded native implementation
that follows the guest's `fs:[0x18]` self-TEB lookup and reads `[TEB+0x24]`,
then performs the zero-argument cdecl return. The promoted bundle is served at
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`. Hashes are JS
`334395ca722616db12717767df694069470b06ccbd5a03b5789ba2ac885f701b`, WASM
`2e24f172ad59c9a5e6baf8d35663b38dbb7c7e0c209d3080609c419da2fd08f2`, and data
`b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`. The tree
remains dirty on `vibe` at `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; dirty
and untracked work was preserved.

Observation: the clean 20-second candidate run logged
`native GetCurrentThreadId @ 3f79cc50`, reached a changing 640x400 canvas
after 7.3s, accepted input (`keys 2, mouse 2`), enabled audio, and reported no
`FATAL`, `BADIP`, `JITBAD`, or `JITBADEIP`. It ended at 401 frames
(`fps: 60.0`); warm intervals reached 985–1,254 FPS. The candidate is now the
canonical control; these noisy samples do not establish a percentage uplift.

Hypothesis: this removes repeated interpretation of a three-instruction
kernel32 thread-ID helper from the remaining DLL load. Continue from the
updated profile, where residual `ntdll`, CRT, and dynamic executable pages
remain measurable.

## 2026-08-29 22:07 IDT: promoted RtlQueryPerformanceCounter hook

The fresh canonical profile showed the `0x3f94xxxx` ntdll page still at
roughly 20–21% of interpreted entries after the frequency hook. Exact misses
clustered around `0x3f9417e0`, which maps to the exported
`RtlQueryPerformanceCounter`. Added a relocation-tolerant skeleton-guarded
native stdcall implementation that writes a monotonic host timestamp in the
guest's verified 10 MHz tick units, returns success, and pops its one
argument. The promoted bundle is served at
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`. Hashes are JS
`0fd827399696ff5e212fde102c1b73c6c5cf38fde27914f4662346d716dd5650`, WASM
`c1132733f28e8f2bbdc900dc8cbd8cb29e6d309598157f62dd59cb1ce942845e`, and data
`b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`. The tree
remains dirty on `vibe` at `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; dirty
and untracked work was preserved.

Observation: the 22-second candidate run logged both
`native RtlQueryPerformanceFrequency @ 3f941820` and
`native RtlQueryPerformanceCounter @ 3f9417e0`, reached a changing 640x400
canvas after 7.7s, accepted input (`keys 2, mouse 2`), enabled audio, and
reported no `FATAL`, `BADIP`, `JITBAD`, or `JITBADEIP`. It ended at 532 frames
(`fps: 59.1`); warm intervals reached 838–1,134 FPS. The previous canonical
control was also stable, so no definitive percentage uplift is claimed from
these noisy single-run measurements.

Hypothesis: replacing the nested performance-counter syscall should reduce
per-frame timing overhead and further drain the ntdll hotspot. Keep this as
the new control and obtain repeated matched runs before attributing the
remaining `0x3f94xxxx` load to other ntdll exports.

## 2026-08-29 22:01 IDT: promoted RtlQueryPerformanceFrequency hook

Added an exact, semantic skeleton-guarded native hook for ntdll's exported
`RtlQueryPerformanceFrequency` at runtime address `0x3f941820`. The guest
routine writes `10000000` as a 64-bit frequency, returns success, and uses
`ret 4`; the native path preserves those stores, return value, and stdcall
stack cleanup. The promoted bundle is served at
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`. Its hashes are JS
`0d330f1bf44a3e8c0efaa11838c250a7cba58b28d740c329ada5ab49b1df79ae`, WASM
`a2c459325de710d7c67dc0e708025b4ee3640e09e289ed98e512d6b308d0d6a1`, and data
`b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`. The tree
remains dirty on `vibe` at `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; dirty
and untracked work was preserved.

Observation: the clean 18-second candidate run logged
`native RtlQueryPerformanceFrequency @ 3f941820`, reached a changing 640x400
canvas after 7.4s, accepted input (`keys 2, mouse 2`), enabled audio, and had
no `FATAL`, `BADIP`, `JITBAD`, or `JITBADEIP`. It ended at 365 frames
(`fps: 63.7`). A same-duration canonical control ended at 402 frames
(`fps: 60.5`); warm interval variance is substantial, so this is recorded as
a safe promotion rather than a definitive FPS percentage.

Hypothesis: this removes repeated interpretation of a small ntdll timing helper
from the `0x3f94xxxx` hotspot. Keep the prior canonical hashes as the control
for a longer repeated benchmark before attributing a large FPS gain.

## 2026-08-29 21:43 IDT: promoted masked-dispatch true-entry hook

Promoted the clean candidate to the canonical bundle served at
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`. The native masked mapper
dispatcher now registers its verified true entry at `0x006324d1`, reproducing
the four guest pushes before the existing dispatcher body; the old hook was
at `0x006324d5`. The promoted artifact hashes are JS
`0536fccb1d2e7d7034ec697dc211ec7c23abc1bfdc2b70b9fda56bc4386c5598`, WASM
`017aaf4802ea576ce628ea7cca73cf3eeed2d5aaf12f298c9a99601a468ef685`, and data
`b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`. The tree
remains dirty on `vibe` at `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; dirty
and untracked work was preserved.

Observation: the clean 24-second browser run logged
`native mvlineasm4 dispatcher @ 006324d1`, reached a changing 640x400 canvas
after 7.5s, accepted Space/input (`keys 2, mouse 2`), enabled audio, and
reported no `FATAL`, `BADIP`, `JITBAD`, or `JITBADEIP`. The final UI sample was
`fps: 69.5`, `frames: 668`; warm intervals reached 899–1,081 FPS, with
`mv=146/12612` and approximately 104–105 interpreted instructions/frame.
These are single-run measurements and are not treated as a definitive FPS
uplift over the noisy control.

Hypothesis: this removes the four-push interpreter boundary on calls entering
the masked mapper and should reduce dispatch overhead without changing the
verified native loop. Keep this bundle as the new control while profiling the
remaining DLL pages and checking the next independent hot boundary.

## 2026-08-29 21:29 IDT: relocatable msvcrt AOT candidate rejected

Tested the existing opt-in CRT translator at
`http://localhost:8837/?WASM_MSVCRT_JIT=1&WASM_TPUT=1&WASM_BADIP=1`, using the
same supplied `netduke32.exe` and the canonical GLX-shim root. The candidate
was built with `MSVCRT_AOT=1` and generated 21,145 relocatable blocks. Its
artifacts were JS/WASM/data sizes 214,740 / 72,035,486 / 191,919,530 bytes;
the promoted canonical hashes remain JS
`62a640cdae7f07baf5942bb2c8f53bcfabfdd79d07a72e8711a2f1b6a2e479d9`, WASM
`89dd3ace679a9f071a3d4d37788ad4dd1fc29f4c5e2ed52c8144ca4e91bf1af2`, and data
`b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`. The tree
remains dirty on `vibe` at `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; dirty
and untracked work was preserved.

Observation: the candidate logged `msvcrt JIT 21145 translated blocks loaded`
then immediately failed with `FATAL worker exception: memory access out of
bounds`; it produced no first frame. The canonical bundle remains unchanged
and continues to render a changing 640x400 frame with input. The generated
`webwine/msvcrt_gen_blocks.c` remains an untracked research artifact; the
default build does not compile or enable its runtime path.

Hypothesis: full-DLL AOT is unsafe until its x87/SSE and ABI boundaries are
handled; do not enable it globally. The next optimization should stay within
the already verified executable mapper/native paths or use a separately
verified individual CRT export.

## 2026-08-29 21:20 IDT: vline dispatcher true-entry hook rejected

Profiled the promoted bundle at
`http://localhost:8806/?WASM_TPUT=1&WASM_IPAGE=1&WASM_HISTO=1&WASM_BADIP=1`.
The artifact hashes were JS
`62a640cdae7f07baf5942bb2c8f53bcfabfdd79d07a72e8711a2f1b6a2e479d9`, WASM
`89dd3ace679a9f071a3d4d37788ad4dd1fc29f4c5e2ed52c8144ca4e91bf1af2`, and data
`b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`. The tree
remains dirty on `vibe` at `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; dirty
and untracked work was preserved.

Observation: the profile reached a changing 640x400 canvas, input (`keys 2,
mouse 2`), and audio, with no `BADIP`, `JITBAD`, `JITBADEIP`, or fatal output.
The hottest interpreted pages were `0x3f940000` (18.6%), `0x00630000`
(15.9%), and `0x00800000` (9.2%). `IMISS top` showed the vline dispatcher
boundary around `0x006324d1`/`0x006326xx`; the existing vline dispatcher hook
was at `0x006320b3`, one byte after its `push edx` prologue. A guarded candidate
registered `0x006320b2` and preserved that push. Candidate hashes were JS
`59cd34a8022ddf9a4441835e2596c0df6faaeb04253d01fac86ae7c1801ed1bf`, WASM
`4331b51bc750dae6a524a7546f8a03e6385ab2bede44e3d990707748d55ee155`, and the
same data hash. In matched 24-second runs the candidate ended at 633 frames
(`fps: 62.7`) versus 638 frames (`fps: 66.5`) for the promoted control; warm
intervals were comparable and noisy. The candidate was not promoted and the
source was restored.

Hypothesis: the `0x0063xxxx` miss load includes more than the one-byte vline
prologue, so this boundary wrapper is not a useful independent lever. The
remaining high-value work is likely a complete masked-dispatch boundary or a
DLL/helper hook selected from a mapped, function-start miss—not another
single-byte mapper adjustment.

## 2026-08-29 21:07 IDT: promoted narrow 0x0080da90 AOT seed

Added `0x0080da90` to the default executable AOT entries in
`webwine/browser/build-node.sh`. This is the proven function entry for the
hot `0x0080db5c` miss region; the generated candidate contains 170,725 blocks
versus 170,676 in the prior bundle. The promoted bundle is served at
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`. Its artifacts are JS
`62a640cdae7f07baf5942bb2c8f53bcfabfdd79d07a72e8711a2f1b6a2e479d9`, WASM
`89dd3ace679a9f071a3d4d37788ad4dd1fc29f4c5e2ed52c8144ca4e91bf1af2`, and data
`b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`. The tree
remains dirty on branch `vibe` at commit
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; unrelated dirty and untracked work
was preserved.

Observation: the 32-second browser run loaded `wasm_x86: JIT 170725 translated
blocks loaded`, reached a changing 640x400 frame at 7.7s, accepted input
(`keys 2, mouse 2`), enabled audio, and produced no `BADIP`, `JITBAD`,
`JITBADEIP`, or fatal output. Warm intervals reported approximately 94.9–95.4%
JIT coverage and 261–266 interpreted instructions/frame. The prior canonical
bundle was 170,676 blocks with JS
`a1b1412606a3973cb5c11f0ed2259f9202bcdbfa14716f6f506e9f7cf7cad20a`, WASM
`a5b360ab091a31aa5156ddf43e1fcb99e9eab33d7e08c6a4b36107d938168d92`, and data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Hypothesis: this is a safe, small reduction in interpreted helper work, but
the run variance is larger than the expected gain; use the promoted bundle for
testing and continue profiling the remaining active DLL and `0x0063xxxx`
misses before claiming a large FPS increase.

## 2026-08-29 20:55 IDT: direct-call leader expansion rejected

Tested an AOT CFG change at
`http://localhost:8832/?WASM_TPUT=1&WASM_BADIP=1`. The candidate added
in-range immediate call targets as generated-block leaders and decoded bounded
windows for targets beyond the linear sweep. Candidate artifacts were JS
`11c2c7deaac824a001f7f7f1982916dff66682f60259af0e86b57643fa076eda`, WASM
`fc928e704048fde3ffd519f1f35fb428afbc4ea75c044d69d04422888b09027e`, and data
`b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`. The tree
remains dirty on branch `vibe` at commit
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; unrelated dirty and untracked work
was preserved.

Observation: the generated table grew from `170676` to `174306` blocks, but
the normal browser run produced no first frame by 24 seconds (`frames=0`,
`render=—`) while the worker remained alive and accepted Space (`keys 2,
mouse 2`). The candidate is rejected and the direct-call leader change was
removed. The preceding exact-miss diagnostic run remained regression-free with
a real changing 640x400 canvas, input (`keys 2, mouse 2`), and no fatal output.

Hypothesis: promoting every direct-call target exposes at least one callee
whose static boundary or generated control-flow context is not safe for this
hybrid chain. Keep direct-call targets as interpreter fallbacks until each
entry has independent boundary proof; continue from narrower runtime-proven
entries.

## 2026-08-29 20:20 IDT: horizontal-mapper epilogue candidate not exercised

Tested a candidate folding the `mhlineskipmodify` six-register epilogue at
`http://localhost:8830/?WASM_TPUT=1&WASM_BADIP=1`. Candidate artifacts were JS
`8d604a899bbfe448823cb4a0053c38094ef57be93ec9bf3abfef437f4d93200a`, WASM
`23afefb88e431d2320de53b18c6c3d7de527304a900e3a28fb1a10995c32f300`, and data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`. The tree
remains dirty on branch `vibe` at commit
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; unrelated dirty and untracked work
was preserved.

Observation: the candidate remained alive through the run and produced stable
real rendering/input, but every reported interval had `mh=0/0`; therefore no
performance or correctness conclusion was possible. The candidate was not
promoted and the source was restored.

Hypothesis: this workload does not reach the horizontal mapper, so the next
optimization must come from the active `0x0063xxxx`/`0x0080xxxx` interpreter
pages or a deterministic gameplay path that exercises `mhlineskipmodify`.

## 2026-08-29 20:11 IDT: mvlineasm4 tail retry rejected again

Retested the opt-in masked-mapper tail candidate at
`http://localhost:8829/?WASM_TPUT=1&WASM_BADIP=1&WASM_EXPERIMENT_MVLINE_TAIL=1`.
Candidate artifacts were JS
`69ea2d15c58b1a6875646f888ec5eaf244c0e2c0ccf67aa1d7e54fa6c0ac05c3`, WASM
`316eb73bf7dce2916694896afac0dba0bc70cdd43d2e2c26c73ff0d1b1e009c4`, and data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`. The tree
remains dirty on branch `vibe` at commit
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; unrelated dirty and untracked work
was preserved.

Observation: this run produced `FATAL worker exception: memory access out of
bounds` before a completed masked-mapper interval (`mv=0/0`); it supplied no
valid tail performance evidence. The candidate was discarded and its source
change removed. The stable port-8806 bundle is unchanged.

Hypothesis: the general browser harness remains nondeterministic before the
first masked call, so the tail is not promotable without deterministic call
replay. The corrected masked dispatcher remains the active performance change.

## 2026-08-29 20:01 IDT: promoted corrected mvlineasm4 dispatcher

Corrected the masked four-column dispatcher model after differential tracing
found that `inc bh` is a `+0x100` operation, not `+0x10000`. The dispatcher at
`0x6324d5` now performs the exact byte-counter setup and enters the existing
verified native loop; it is enabled by default with
`WASM_NO_MVLINE_DISPATCH=1` as rollback. The promoted bundle is served at
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`. The tree remains dirty on
branch `vibe` at commit `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; unrelated
dirty and untracked work was preserved.

The served artifacts are JS
`a1b1412606a3973cb5c11f0ed2259f9202bcdbfa14716f6f506e9f7cf7cad20a`, WASM
`a5b360ab091a31aa5156ddf43e1fcb99e9eab33d7e08c6a4b36107d938168d92`, and data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: the corrected opt-in candidate at
`http://localhost:8828/?WASM_TPUT=1&WASM_BADIP=1&WASM_EXPERIMENT_MVLINE_DISPATCH=1&WASM_TRACE_MVDISPATCH=1`
and the promoted default run both armed `native mvlineasm4 dispatcher @
006324d5`, rendered a real changing 640x400 canvas, accepted input (`keys 4,
mouse 2`) with audio on, and produced no `BADIP` or fatal output. The
corrected candidate sustained about 144–145 masked calls and 3.5K iterations
per interval; the promoted run sustained approximately 950–999 guest
flips/sec with `kinsn/frame=188–190`. The earlier uncorrected candidate is
rejected because its counter model produced runaway native iterations.

Hypothesis: the dispatcher bypass is now semantically correct and removes a
real interpreter boundary, but its isolated FPS gain is within current run
variance. Continue from the post-mapper interpreter-page profile for the next
optimization rather than claiming a large gain from this change.

## 2026-08-29 19:59 IDT: promoted mvlineasm4 dispatcher bypass

Added a skeleton-checked native implementation for the hot masked mapper
dispatcher at `0x6324d5`. It reproduces the five-register preamble, patches
the current self-modified loop immediates, performs the byte-exact `inc cl` /
`inc bh` counter setup, pushes EBP, and enters the existing native loop. The
promoted bundle is served at
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; `WASM_NO_MVLINE_DISPATCH=1`
disables the bypass. The tree remains dirty on branch `vibe` at commit
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; unrelated dirty and untracked work
was preserved.

The served artifacts are JS
`a1b1412606a3973cb5c11f0ed2259f9202bcdbfa14716f6f506e9f7cf7cad20a`, WASM
`a5b360ab091a31aa5156ddf43e1fcb99e9eab33d7e08c6a4b36107d938168d92`, and data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: the opt-in candidate and promoted default run both armed
`native mvlineasm4 dispatcher @ 006324d5`, rendered a real changing 640x400
canvas, accepted input (`keys 4, mouse 2`) with audio on, and produced no
`BADIP` or fatal worker output. The candidate sustained roughly 144–145
masked calls and 3.5K iterations per interval, with `kinsn/frame=185–193`
and approximately 950–999 guest flips/sec after warm-up, compared with the
prior vline-only bundle's approximately 187–189 instructions/frame.

Hypothesis: both four-column mapper prologues are now bypassed; the next
performance target should be measured from the remaining interpreter pages or
the masked loop's internal boundary, not another unverified tail fold.

## 2026-08-29 19:15 IDT: guarded mvlineasm4-tail retry still not promotable

Retried the opt-in tail candidate at
`http://localhost:8824/?WASM_TPUT=1&WASM_BADIP=1&WASM_EXPERIMENT_MVLINE_TAIL=1`.
Candidate artifacts were JS
`ae57f621729f1162aee98c7488f2916672a518aa020062be9abdbe8e12b5194d`, WASM
`86d98316e0ae4f52024cd6eb5d7cde95741a4c03c41319481f262cd24cb7868f`, and data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`. The tree
remains dirty on branch `vibe` at commit
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; unrelated dirty and untracked work
was preserved.

Observation: this run emitted fatal worker output before any completed masked
mapper sample (`mv=0/0`); it was not promoted. The opt-in tail code and the
temporary entry trace were removed, leaving the stable port-8806 bundle and
source path unchanged.

Hypothesis: timing-dependent startup/scene transitions are still confounding
the candidate, so no correctness or FPS conclusion is drawn from this run.
The next useful experiment must force or replay a known `mvlineasm4` call,
rather than repeatedly testing the tail through the general browser harness.

## 2026-08-29 18:57 IDT: second mvlineasm4 tail candidate not promoted

Retested the masked-mapper tail candidate over the longer harness at
`http://localhost:8822/?WASM_TPUT=1&WASM_BADIP=1`, using the same supplied
`netduke32.exe`. Candidate artifacts were JS
`0929b56397ad8f85be227047e1c4d6a0285c17c70a08adbf16493c21bbc57643`, WASM
`9165ad5b699c4ca63f37b4f3df1332ac5e505a69acbe235ec84d880d18b9ccf4`, and data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`. The tree
remains dirty on branch `vibe` at commit
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; unrelated dirty and untracked work
was preserved.

Observation: the candidate produced fatal worker output during this run and
never recorded a completed `mvlineasm4` call (`mv=0/0` in the final samples),
so it supplied no valid performance evidence. It was discarded, the source
tail change was removed again, and the port-8806 served bundle was left
unchanged.

Hypothesis: the current test timing can fail before the target mapper is
entered, but the repeated failure means the masked tail remains unfit for
promotion. A direct differential trace must first establish the native loop's
entry and exit state under a guaranteed `mvlineasm4` call.

## 2026-08-29 18:48 IDT: bounded mvlineasm4 entry trace did not exercise the target

Ran the diagnostic candidate at
`http://localhost:8821/?WASM_TPUT=1&WASM_BADIP=1&WASM_TRACE_MVENTRY=1`.
Candidate artifacts were JS
`c3541cc941370c79c2f4ebeb435e9952714b1aff1cec57f426a2dc93b8634bd7`, WASM
`06a22dcc242f6daa7fd71fa4d1e9abd94ee0065642468943edb8fd0602223485`, and data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`. The tree
remains dirty on branch `vibe` at commit
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; unrelated dirty and untracked work
was preserved.

Observation: this run produced no `MVENTRY` line and no masked-mapper counter
increment; it stalled during an earlier scene phase (`flips=4360`, then
`fps=0`) without a fatal worker message. The trace-only instrumentation was
removed and the candidate was not promoted. The stable served bundle remains
the vline dispatcher/tail bundle at port 8806.

Hypothesis: the current short harness is timing-sensitive and did not reach
`mvlineasm4`, so this diagnostic is inconclusive about its entry invariant.
Use a longer canonical active-scene run or a direct address probe before
changing the masked mapper again.

## 2026-08-29 18:39 IDT: differential probe confirms mvlineasm4 native-loop invariant remains unresolved

Ran a traced candidate at
`http://localhost:8820/?WASM_TPUT=1&WASM_BADIP=1&WASM_TRACE_MVTAIL=1` while
investigating the rejected masked-mapper tail fold. Candidate artifacts were
JS `d58788be45385fa2e57888b3ab87fc469f21bf9ec28b0d23a68e44bdd1c46536`, WASM
`e61989ecadf3932accd958f02efd572826b633aec51bc6d1e9da43240d56158f`, and data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`. The tree
remains dirty on branch `vibe` at commit
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; unrelated dirty and untracked work
was preserved.

Observation: this candidate reached the real canvas, but stopped progressing
when the first masked phase began: guest flips plateaued at 2718, `mv=0/0`,
and no `MVTAIL` trace was emitted, proving the fault/hang occurs before the
native hook reaches its exit. The candidate was discarded; the served runtime
was not changed and the source was restored to the stable vline-only bundle.

Hypothesis: the existing `mvlineasm4` native loop has an earlier counter or
entry-state mismatch under this scene, so its tail cannot be optimized safely
until that invariant is differentially checked. Do not retry the tail fold
without tracing the guest/native state at loop entry and each counter boundary.

## 2026-08-29 18:31 IDT: rejected mvlineasm4 tail fold after runtime fault

Tested a candidate that folded the masked four-column mapper's `endmvlineasm4`
tail into the native hook at
`http://localhost:8819/?WASM_TPUT=1&WASM_BADIP=1`. Candidate artifacts were JS
`f80f0cc99eb4c3ba12d36af70ae457478945ac4351464d4c382b0de1f8cd76e4`, WASM
`9165ad5b699c4ca63f37b4f3df1332ac5e505a69acbe235ec84d880d18b9ccf4`, and data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`. The tree
remains dirty on branch `vibe` at commit
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; unrelated dirty and untracked work
was preserved.

Observation: the candidate reached the real changing canvas and initial input
path, but produced `FATAL worker exception: memory access out of bounds` when
the first masked-mapper phase began (`mv=0/0` immediately before the fault).
It was not promoted; the source change was removed and the served runtime
remains the stable vline bundle from the preceding checkpoint.

Hypothesis: the masked mapper's entry/exit boundary has a different live-stack
or counter invariant than the static tail suggested. Revisit it only with a
direct differential trace of the guest tail; continue performance work on a
separately measured path.

## 2026-08-29 18:22 IDT: folded vlineasm4 tail and return epilogue

Extended the native `vlineasm4` path through its post-loop state publication
and six-register return epilogue. The native path now writes the two renderer
accumulator pairs, preserves the final `add` lazy flags, restores the guest
stack/registers, and resumes at the caller return address. The promoted bundle
is served at `http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`. The tree remains
dirty on branch `vibe` at commit `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`;
unrelated dirty and untracked work was preserved.

The served artifacts are JS
`7f42fdcba9bc7bd98bbc1e8cdfd0e6e806293b2fa2caf3d18a1ce7f969efb795`, WASM
`832c0c1fc3b217828cf5f2c9d799762ef7a4853b7d5e999a93e99eeec9865df8`, and data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: the 20-second candidate reached a real changing 640x400 canvas,
accepted keyboard/mouse input, and produced no `BADIP` or fatal worker output.
After warm-up it sustained approximately 991–998 guest flips/sec and
`kinsn/frame=192–194`, down from approximately `200–201` with the dispatcher
only. The native vline dispatcher remained armed at `006320b3`; vline samples
continued at roughly 66 calls and 13.2K iterations per interval.

Hypothesis: the vline boundary overhead is now substantially reduced; the next
independent target is the analogous `mvlineasm4` tail/epilogue or its SMC
dispatcher, followed by another measurement before considering SSE/x87 work.

## 2026-08-29 18:14 IDT: promoted the vlineasm4 dispatcher prologue bypass

Added a skeleton-checked native implementation for the hot `vlineasm4`
dispatcher prologue at `0x6320b3`. It reproduces the six-register stack
sequence, reads the renderer's current self-modified immediates, and enters the
already verified native four-column loop. The bypass is enabled by default;
`WASM_NO_VLINE_DISPATCH=1` is the rollback switch. The promoted bundle is
served at `http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`. The tree remains
dirty on branch `vibe` at commit `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`;
unrelated dirty and untracked work was preserved.

The served artifacts are JS
`887e743be9ffa94f4fe7f3db933d904fda4872f756289b41b65e5380a74381df`, WASM
`c4a9c59ab2529eafc6718073ce1c40da61e160bb447cefd99f4884ddb5c02e2a`, and data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: the opt-in 20-second candidate and the promoted default run both
reached a real changing 640x400 canvas; input became ready and no `BADIP` or
fatal worker output appeared. The candidate armed with
`native vlineasm4 dispatcher @ 006320b3` and sustained approximately
`998–1000` guest flips/sec after warm-up, with `kinsn/frame=200–201`; the
promoted run reached `1028.8` flips/sec by its final sample while still
warming into the active scene. The first implementation was correctly
rejected by its guard and later failed only because it modeled the guest stack
boundary four bytes incorrectly; that candidate was never promoted.

Hypothesis: this removes a real interpreter boundary but is a modest gain, so
the next measurable target remains the vline/mvline tail or another hot SMC
dispatcher rather than SSE/x87 wholesale translation.

## 2026-08-29 17:34 IDT: folded both single-column mapper epilogues

Extended the verified native mapper path so `vlineasm1`, like `mvlineasm1`,
restores its callee-saved registers and returns directly from the native hook.
The promoted bundle is served at
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`. The tree remains dirty on
branch `vibe` at commit `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; unrelated
dirty and untracked work was preserved.

The served artifacts are JS
`8699867710c660ec0d097889292f1f981b653f7f556c64b1a36833d36b710abe`, WASM
`642ce09f56f349f15a38cc178523c5bd28e40ae1e5fb3aff980b55795e62a092`, and data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: the browser regression reached a real changing 640x400 canvas,
reported `input: ready` (`keys 2, mouse 2`) and audio on, and produced no
`BADIP` or fatal worker output. After warm-up, guest samples reported
`kinsn/frame=199–200`, `JITCOV jit_frac=88.8–89.0%`, and approximately
980–995 flips/sec. The preceding bundle with only the `mvlineasm1` epilogue
fold measured about `198–199` instructions/frame in its cleanest intervals;
this vline change is therefore promoted for boundary reduction and stability,
but its isolated FPS gain is not yet statistically separated from run variance.

Hypothesis: the remaining measurable work is in the SMC dispatcher and Wine DLL
interpreter paths, not these two single-column epilogues. The next effort will
target a separately measured path.

## 2026-08-29 17:26 IDT: folded mvlineasm1 epilogue removes another interpreter boundary

Promoted the corrected `mvlineasm1` hook to complete its guest epilogue
natively, then tested the canonical bundle at
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`. The tree remains dirty on
branch `vibe` at commit `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; unrelated
dirty and untracked work was preserved.

The served artifacts are JS
`cd211d8fff1adc527c05a42925264dfa5a4a40427458f99e3d70a91edefb41f8`, WASM
`a3d20c5d7ae1492b6eae1419afb327f9a1a4d10d0597e0b3de6b4199859fc411`, and data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: the browser run reached a real changing 640x400 canvas, reported
`input: ready` (`keys 2, mouse 2`) and audio on, and remained free of `BADIP`
and fatal worker output. After warm-up, guest samples settled at
`kinsn/frame=198–199`, `JITCOV jit_frac=88.9–89.1%`, and roughly 870–990
flips/sec; the prior corrected-hook run was about `206–208` interpreted
instructions/frame and `87.7–88.0%` JIT coverage. The native hook now restores
EBP/EDI/ESI/EDX/ECX/EBX, preserves the loop result in EAX, restores ESP, and
loads the guest return EIP directly.

This is promoted as a measured interpreter-boundary reduction, not as a claim
that the overall browser FPS goal is complete. Hypothesis: the remaining load
is distributed across the SMC dispatcher and Wine DLL paths; the next step is
to measure those paths independently before attempting another native shortcut.

## 2026-08-29 17:16 IDT: explicit double-classifier AOT entry measured as no gain

Tested a candidate that promoted executable entry `0x00806740` (the body
following the profiler hotspot at `0x0080673d`) into the generated AOT table at
`http://localhost:8811/?WASM_TPUT=1&WASM_BADIP=1`. The tree remains dirty on
branch `vibe` at commit `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; unrelated
dirty and untracked work was preserved.

Candidate artifacts were JS
`eed45e0c1cd61d676c2abaddbbc171b5d6f6a8f127c18d6ebc07d1953e62e817`, WASM
`8348dd63ddafb8af9119d6f7743b3badde610a580c9eedb0124fce341dd0c707`, and data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: the candidate loaded `170732` blocks versus `170676` for the
matched control and reached a real 640x400 changing canvas with input ready
(`keys 2, mouse 2`), but both runs converged to `194–198` interpreted
instructions/frame and `89.4%` JIT coverage in the active interval. There was
no `BADIP` or fatal exception. The candidate is rejected and the build default
was restored; no FPS gain is claimed.

Hypothesis: the profiler sample at `0x0080673d` is a return/entry artifact or
an already-covered path, not a missing callable AOT leader. Continue with a
different measured hotspot rather than promoting isolated helper entries.

## 2026-08-29 17:00 IDT: corrected mvlineasm1 hook survives a 30-second render run

Tested the default mapper path with the transition diagnostic enabled at
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`. The tree remains dirty on
branch `vibe` at commit `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; unrelated
dirty and untracked work was preserved.

The served artifacts are JS
`af1fbcc332a435740b81586705ed0148ee9c65c6d36f216b2a5781383b394484`, WASM
`5b2288b08c7c64d9fac7ba0282798f7a32e508e9d713149ff1f85a8a9cf04378`, and data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: after the 7.2-second first frame, the run presented continuously
through the 30-second capture: the final page sample was a real 640x400 canvas
with `frames: 1091`, `fps: 60.8`, `input: ready` (`keys 4, mouse 2`), and audio
on. Guest `FPSSAMPLE` intervals sustained about 978–997 flips/sec from 20 to
33 seconds, with `mv1=69–70` calls per interval and no `BADIP` or fatal worker
exception. The page FPS is screenshot-observer sampling and is not comparable
to the guest flip counter.

The fix is now verified as both live and stable: `mvlineasm1` reads its
self-patched stride immediate at `b+0x18` and resumes at the loop epilogue
`b+0x21` (`0x631dd1`) instead of returning past the epilogue. This removes the
previous low-memory corruption in the tested workload. Hypothesis: the next
performance lever is still SSE/x87 translation or another measured interpreted
region; the mapper hook itself is no longer the immediate correctness blocker.

## 2026-08-29 16:46 IDT: runaway low-memory transition localized

Ran the canonical bundle with the opt-in transition diagnostic at
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`. The tree remains dirty on
branch `vibe` at commit `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; unrelated
dirty and untracked work was preserved.

Current artifacts are JS
`af1fbcc332a435740b81586705ed0148ee9c65c6d36f216b2a5781383b394484`, WASM
`6d88842652fc9b8ebc4065cf557064fbb54cb16cc1df1a7e8248f19c1bc8cb26`, and data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: the run rendered a real 640x400 changing canvas (`non-black=8`,
`distinct frames=3`), accepted input (`keys 2, mouse 2`), and then reported
`BADIP transition prev=00631e42 eip=0032fad8 esp=0032f808 ...
ret=03a900b8`, followed by `FATAL worker exception: memory access out of
bounds`. The return address `0x0032fad8` is in the same low-memory region,
`0x7d0` above the recorded stack pointer; the bytes previously sampled there
are data-like, so this is a concrete
return-target/stack-state lead. It occurs after the NP2 loop's `ret` address
(`0x00631e42`) even when NP2 is disabled; it is not proof that the experimental
hook is safe or the cause of the fault.

Hypothesis: the largest remaining interpreted load is a bad or dynamically
generated return path around the SMC mapper caller, and fixing its provenance
could remove the later `JITCOV jit_frac=0%` spin. The next step is to capture
the call instruction and stack writes that place `0x0032fad8` there before
attempting another acceleration.

## 2026-08-29 16:41 IDT: corrected NP2 state preservation is stable but not a FPS lever

Tested the explicitly opt-in corrected mapper implementation at
`http://localhost:8806/?WASM_TPUT=1&WASM_EXPERIMENT_NP2=1`, then ran the
canonical path with `WASM_HISTO=1` and `WASM_STALLBT=1`. The tree remains dirty
on branch `vibe` at commit `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; all
unrelated dirty and untracked work was preserved.

Current served artifacts are JS
`af1fbcc332a435740b81586705ed0148ee9c65c6d36f216b2a5781383b394484`, WASM
`8f383aca5e89f99252090d9f4d6e1b39263298f321123427b4ec08458e419c0b`, and data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: the candidate armed both NP2 loops, produced a real 640x400
changing canvas (`non-black=8`, `distinct frames=4`), accepted input (`keys 2,
mouse 2`), and had no fatal exception through 18 seconds. The measured
`np2v`/`np2m` counters remained zero in normal frame intervals, except for one
masked iteration during a transient interval; therefore this path cannot
explain the remaining load or produce a meaningful FPS gain in this workload.
The histogram’s active render interval was dominated by ordinary integer
opcodes (`89`, `8b`, `83`, `0f`, `74`, `85`, `75`, `8d`) rather than a missing
SSE/x87 opcode. The long stall later reported `STALLBT eip=0032fad5` with
return `07060504`, which is a separate invalid/dynamic low-memory execution
state after rendering stopped, not evidence that the NP2 hook is correct for
all callers.

The NP2 implementation remains opt-in and is not promoted as a performance
change. Hypothesis: the next useful work is to trace the dynamic low-memory
execution/return path or accelerate the remaining non-executable Wine/DLL
load, rather than add more static mapper hooks.

## 2026-08-29 16:14 IDT: masked non-power-of-two hook rejected; safe bundle restored

The non-power-of-two single-column mapper experiment was investigated with
corrected self-patched-immediate offsets, instruction ordering, trace logging,
and address guards at
`http://localhost:8806/?WASM_TPUT=1` (with matched A/B runs using
`WASM_NO_MVLINE1NP2=1` and `WASM_NO_VLINE1NP2=1`). The tree remains dirty on
branch `vibe` at commit `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; unrelated
dirty and untracked work was preserved.

Observation: the vline-only run completed without a fatal, while the
masked-only and both-enabled runs ended in
`FATAL worker exception: memory access out of bounds`. Trace-only execution
showed valid first-entry state (`ecx=00000066`, `ebp=00000000`,
`ebx=03a900b5`, `esi=00000069`, `edi=0050a14f`, `stride=00000140`,
`pal=03a98035`), so the remaining mismatch is not explained by the initial
pointer values. All candidate hooks are removed and rejected; no FPS gain is
claimed.

The canonical safe bundle was rebuilt and is served at the same URL. Its
artifacts are JS
`af1fbcc332a435740b81586705ed0148ee9c65c6d36f216b2a5781383b394484`, WASM
`bbbb9c0920207daec2cf45af5f6aa4e733cf2920e5ed79b8b6524317b7aad1ad`, and data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.
Hypothesis: a useful native replacement needs differential capture around the
complete SMC mapper prologue, or the next effort should target SSE/x87.

## 2026-08-29 14:54 IDT: corrected non-power-of-two mapper hook rejected

Retested the SMC mapper loop with the corrected high-word `mul` texture
address calculation at
`http://localhost:8806/?WASM_TPUT=1`. The tree remains dirty on branch `vibe`
at commit `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; unrelated work was
preserved.

Candidate artifacts were JS
`af1fbcc332a435740b81586705ed0148ee9c65c6d36f216b2a5781383b394484`, WASM
`9fa456c8ac7ff000c4e3e153207ce932bf897952d57da7a3b78f9875a06a9d25`, and data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: only `mvlineasm1nonpow2` passed the guard and armed. The run
reached a real 640x400 changing canvas with `input: ready` (two keys, two
mouse events), but ended with `FATAL worker exception: memory access out of
bounds`. A matched run with both new hooks disabled completed without that
fatal and retained real frames/input. The candidate is removed and rejected.
Hypothesis: the loop-entry register state still depends on the complete guest
prologue/stack frame, so hand-written replacement remains unsafe without
per-call differential capture and verification.

## 2026-08-29 14:43 IDT: non-power-of-two mapper hook rejected

Tested native replacements for the SMC loop entries
`vlineasm1nonpow2` (`0x631d59`) and `mvlineasm1nonpow2` (`0x631e12`) at
`http://localhost:8806/?WASM_TPUT=1`. The Wine tree remains dirty on branch
`vibe` at commit `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; unrelated dirty
and untracked work was preserved.

Candidate artifacts were JS
`af1fbcc332a435740b81586705ed0148ee9c65c6d36f216b2a5781383b394484`, WASM
`a157b5525072659f5c2053bf5efaf597205f2e2e9b94f0a8844c0fe53bb24a14`, and data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: both hooks passed their startup skeleton guards, the browser
reached a real 640x400 changing canvas with `input: ready` (two keys, two
mouse events), and active samples reached 1,689 FPS. The run then terminated
with `FATAL worker exception: memory access out of bounds`; therefore the
candidate is rejected and the source hook has been removed. Hypothesis: the
non-power-of-two palette displacement/coordinate state cannot be inferred from
the loop entry alone; it needs differential verification against the complete
guest prologue before revisiting.

## 2026-08-29 14:41 IDT: corrected matrix-uniform deduplication rejected

Corrected the earlier probe to use the four guest arguments recorded for
`glUniformMatrix4fv` and tested the opt-in cache at
`http://localhost:8806/?WASM_TPUT=1&WASM_GLCOUNT=1&WASM_GLDEDUPE=1`. The Wine
tree remains dirty on branch `vibe` at commit
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; unrelated dirty and untracked work
was preserved.

The rebuilt candidate artifacts were JS
`af1fbcc332a435740b81586705ed0148ee9c65c6d36f216b2a5781383b394484`, WASM
`b0f2dd1498efef1e5017604229d94c0e6cc7a924a17bab133e558a9b55705095`, and data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

Observation: the run produced a real 640x400 canvas with changing frames
(194 total, first frame 8.3s), `input: ready` with two keys and two mouse
events, and `GLDEDUPE skips=1` while `glUniformMatrix4fv` reached 8,422 calls;
`glDrawArrays` reached 8,413 calls. Active samples ranged from 1,008 to
1,404 FPS before the known intermittent stall. The cache was reverted because
one skip is negligible and no gain was demonstrated. Hypothesis: the useful
remaining work is guest-side SMC/SSE-x87 execution or draw-call cost, not
repeated matrix uploads.

## 2026-08-29 14:24 IDT: initial matrix-uniform deduplication probe invalidated

An initial opt-in `WASM_GLDEDUPE=1` probe for byte-identical
`glUniformMatrix4fv` uploads was run. The Wine tree
remains dirty on branch `vibe` at commit
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; unrelated dirty and untracked work
was preserved.

Candidate URL was
`http://localhost:8806/?WASM_TPUT=1&WASM_GLCOUNT=1&WASM_GLDEDUPE=1`, with JS
SHA-256 `af1fbcc332a435740b81586705ed0148ee9c65c6d36f216b2a5781383b394484`,
WASM `744ec9be6f7532d41980c8f1e85b35ffdfdfa49495286c7e20a19caac7d93585`,
and data `d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

The run logged WebGL 3.3, a 640x400 canvas, `input: ready` with two keys,
nine non-black samples, and five distinct screenshots. However, the probe
checked `nargs == 5`, while the actual `nd_glext` metadata records four guest
arguments for this API. Therefore its `skips=0` result is invalid evidence
about repetition. The experiment was reverted pending a corrected probe; no
performance conclusion is drawn from this run.

## 2026-08-29 14:16 IDT: GL function-pointer cache rejected

Added an opt-in `WASM_GLCOUNT` diagnostic and tested a candidate that cached
each resolved native GL function pointer, removing the dispatch-table load from
the hot thunk. The Wine tree remains dirty on branch `vibe` at commit
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; unrelated dirty and untracked work
was preserved.

Candidate bundle at `http://localhost:8806/?WASM_TPUT=1&WASM_GLCOUNT=1`:
JS `af1fbcc332a435740b81586705ed0148ee9c65c6d36f216b2a5781383b394484`,
WASM `b5897e13cc18a4175e19e4ef0ff9c4125a066444f8db4bca87d1deb3aa305c09`,
data `d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.
The preserved pre-change control at
`http://localhost:8807/?WASM_TPUT=1` used JS
`b87553df7e478fe3a0b2828b6fdc876c57435e1f8c610ca37f4defeb44c3d4d2`, WASM
`d0a9aa71795134936ba216143711b32dab4e591a04f6da1235ead31345d8f987`, and the
same data hash.

Both runs logged the WebGL 3.3 driver, `w=640 h=400`, non-black changing
screenshots, and `input: ready` with two keys received. The candidate’s active
intervals were generally slower than control (for example `1295` vs `1341`
FPS and `983` vs `1052` FPS); GL counts stayed about one
`glUniformMatrix4fv` and one `glDrawArrays` per draw (~1,700 each interval).
The cache was reverted. Observation: dispatch-table lookup is not the useful
lever; hypothesis: further FPS work must reduce draw count/host WebGL work or
accelerate a remaining guest render path.

## 2026-08-29 13:59 IDT: GL thunk argument unrolling rejected

Tested straight-line argument loads for the common 0–4-argument OpenGL thunk
signatures, compared with the existing small runtime loop. The Wine tree
remains dirty on branch `vibe` at commit
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; unrelated dirty/untracked work was
preserved.

The candidate was stable, but reverse-order Node intervals measured roughly
`240 MIPS` versus `243 MIPS` for the loop control, with identical rendered
`kinsn/frame` ranges. The unrolled bridge was reverted; the existing generic
GL thunk remains the default. No browser artifact was promoted from this
experiment.

Observation: reducing a few argument-load branches does not overcome the
bridge’s host-call and register-pressure costs. Hypothesis: the next useful
GL optimization must batch or eliminate calls, not merely rearrange the
per-call marshalling loop.

## 2026-08-29 13:54 IDT: O3 interpreter build not promoted

Built the current executable-AOT/interpreter configuration with `XOPT=-O3`
and compared it in both orders with the existing `-O2` configuration. The
Wine tree remains dirty on branch `vibe` at commit
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; unrelated dirty/untracked work was
preserved.

The reverse-order active Node intervals measured O3 at approximately `240`
MIPS and O2 at approximately `244` MIPS; `kinsn/frame` stayed in the same
`125-151` range. The first order favored O3 by a few MIPS, so the result is
within the known host/scene noise rather than a demonstrated improvement. The
build scripts retain the O2 default and no browser artifact was promoted from
this test.

Observation: compiler-level O3 does not provide a reliable FPS/load gain on
the current large `run()`/AOT module. Hypothesis: further improvement needs
to remove interpreter dispatch work or accelerate a measured renderer path,
not simply increase optimization level.

## 2026-08-29 13:49 IDT: multi-entry AOT lookup cache rejected

Tested a 16-entry direct-mapped cache for repeated executable AOT lookup
addresses after dynamic exits, leaving guest semantics and code invalidation
unchanged. The Wine tree remains dirty on branch `vibe` at commit
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; unrelated dirty/untracked work was
preserved.

The candidate and prior one-entry lookup build used the same generated table
(`170676` translated blocks). In active Node render intervals the candidate
measured about `240 MIPS` (`kinsn/frame=125-150`) versus about `244 MIPS`
(`kinsn/frame=125-149`) for the control. The candidate was reverted; the
original one-entry cache remains. No browser bundle was promoted from this
experiment.

Observation: adding live lookup-cache state loses more to register/cache
pressure than it saves in hash probes. Hypothesis: a useful decoded-block cache
must eliminate interpreter dispatch work itself and include generation-aware
SMC invalidation; another lookup-only cache is not worthwhile.

## 2026-08-29 13:52 IDT: indexed DLL relocation fixed, whole-DLL AOT still rejected

The msvcrt prototype was corrected so SIB addresses with no base register,
including `[index*4 + table]`, receive the module slide. This removes the
specific missing-relocation case found at the earlier NULL-return site. A
rebuild with `MSVCRT_AOT=1` still failed in the opt-in Node run with a WASM
`memory access out of bounds` during startup, so the full-DLL table remains
rejected. The Wine tree is dirty on branch `vibe` at commit
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; unrelated work was preserved.

The default path remains build-time gated and is unaffected. Observation: DLL
relocation coverage is not yet sufficient for broad AOT promotion; hypothesis:
the next viable step is a relocation-aware function whitelist with explicit
PE relocation records, not another whole-section sweep. No browser artifact
was promoted from this run.

## 2026-08-29 13:38 IDT: ntdll floor native-return hook rejected

The profiler’s repeated ntdll `floor` samples were mapped to the loaded export
entry and tested with an opt-in `WASM_FAST_MATH=1` hook implementing the cdecl
double-argument / x87 `ST(0)` return convention. The Wine tree remains dirty
on branch `vibe` at commit `ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; all
unrelated work was preserved.

The hook registered at `3f967580` and the 20-second Node run stayed alive with
no `JITBAD`, `JITBADEIP`, `FATAL`, or NULL output. Its active interval measured
about `230 MIPS` versus `243 MIPS` for the same build without the hook, with
the same `kinsn/frame` range (`125-149`) during rendered intervals. It is not
promoted: the host call plus x87 stack transition is slower or neutral, and the
short run does not justify a production ABI shortcut. No browser bundle was
served from this experiment.

Observation: an export-level native math hook is not the next FPS lever. The
remaining work should target a hot path that removes substantial interpreted
instruction count or a proven mapper loop; the opt-in hook remains available
only as a diagnostic experiment.

## 2026-08-29 13:27 IDT: export-CFG and integer-only msvcrt AOT still rejected

The module-AOT prototype was tightened in two ways: DLL code is now discovered
from exported function seeds with recursive direct-CFG edges instead of a
linear sweep, and `--no-fp` forces every x87/SIMD instruction through the
interpreter. Both variants still reproduced the same NULL transfer at
`eip=3ee3fed0` when enabled with `WASM_MSVCRT_JIT=1`, so neither is promoted.
The source remains dirty on branch `vibe` at commit
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; unrelated dirty/untracked work was
preserved.

The default build was compile-checked without `WEBWINE_MSVCRT_AOT`; the
experimental generated table is now excluded at build time unless
`MSVCRT_AOT=1` is explicitly supplied. No default runtime behavior changes.
The next implementation target is a narrower, export/function-level helper
with an independently checked ABI, rather than whole-DLL block dispatch.

## 2026-08-29 12:43 IDT: active profiler map after AOT coverage expansion

Ran the diagnostic PROFILE=1 candidate against the custom worker bundle at
`http://localhost:8803/?WASM_DIAG=1&WASM_TPUT=1&WASM_PROF=1&WASM_IPAGE=1`.
The Wine tree remains dirty on branch `vibe` at commit
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; all existing dirty and untracked
work was preserved.

The diagnostic artifacts were `webwine-bw.js` SHA-256
`58ba68a08b545629713a228d0a0ab192ff222b1a86436836eb62a801b7e60de9`,
`webwine-bw.wasm` SHA-256
`f2fba49f9d4d1bd12aed07c29c1865b6778a4917bbd0195ad38a1140637e08df`, and
`webwine-bw.data` SHA-256
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.

The run reached its first frame at 9.3s and reported `frames=88` by 24s on a
640x400 worker canvas. It remained alive without a fatal exception, accepted
Space input, and the guest log contained `wasm_input: input target window
0x20028`. Active-window samples included `JITCOV ... jit_frac=76.0%
total=43.8M/s ... kinsn/frame=42`; `PROF TOTAL 8812 dropped 0` was captured.
IPAGE hotspots were distributed across msvcrt (`3ee0/3ee3`), ntdll
(`3f92/3f93`), the executable (`0080`), gdi32 (`3e3f/3e3b`), and smaller
kernelbase/user32/OpenGL pages. Named samples included ntdll `floor`,
`RtlAllocateHeap`, and `towupper`, msvcrt `perror`, `_strnicmp_l`, and
`_strtoi64_l`, and gdi32 `ArcTo`/`WidenPath`.

Observation: this run is stable and confirms the remaining active work is
spread across several Wine DLLs and executable code; late samples dominated by
`0x00320000` occurred after rendering had stopped and are not treated as an
FPS hotspot. Hypothesis: a single new native mapper or executable entry is
unlikely to remove the remaining load. The next focused performance effort is
to add module-aware AOT for a measured DLL/helper subset, beginning with a
runtime-base/RVA design and differential verification rather than guessing
native replacements for floating-point or CRT functions. The canvas is real
worker frame status, not page-side pixel readback proof.

## 2026-08-29 12:55 IDT: msvcrt module-AOT prototype rejected from default path

As the first module-aware AOT prototype, `x86toc.py` now supports a relocation
symbol and generated-name prefix. It generated 30,387 msvcrt `.text` blocks
from the Wine PE (`0x10001000-0x1007c000`), and `wasm_x86.c` has a separate
runtime-base/RVA hash table. The source remains dirty on `vibe` at
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; unrelated work was preserved.

The build completed successfully in `/tmp/webwine-msvcrt`, and the opt-in
table loaded as `msvcrt JIT 30387 translated blocks loaded`. The normal default
path was then rebuilt and exercised with `TPUT=1`: it rendered successfully
(first reported frames at 7.7s, up to 1,441 FPS in the unlocked headless
interval, `kinsn/frame=125-1507` across scene/stall transitions) with no
`JITBAD`, `JITBADEIP`, or fatal output. The msvcrt table is disabled by default
and requires `WASM_MSVCRT_JIT=1`.

The safe-default browser bundle was built from that same tree and served at
`http://localhost:8804/?WASM_DIAG=1`. Its artifacts were `webwine-bw.js`
SHA-256 `b87553df7e478fe3a0b2828b6fdc876c57435e1f8c610ca37f4defeb44c3d4d2`,
`webwine-bw.wasm` SHA-256
`d0a9aa71795134936ba216143711b32dab4e591a04f6da1235ead31345d8f987`, and
the unchanged data SHA-256
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`.
The 16-second browser run reached first frame at 7.2s, reported
`frames=294` and `render: 640×400`, produced nine non-black samples and four
distinct screenshots (including `fa7eff88` at 10s), and remained alive. The
worker accepted the Space event, but this server did not provide COOP/COEP, so
the probe correctly reported SharedArrayBuffer input as disabled; this is a
server-header limitation, not input success.

Observation: enabling the prototype caused a NULL transfer during a
floating-point-heavy msvcrt routine at `eip=3ee3fed0`, followed by top-level
thread exit. Hypothesis: translating the full DLL needs stricter function-entry
and x87/SIMD/ABI validation (the generator reports `lea`, SIMD, and x87
fallbacks); it is not safe to promote as-is. No browser bundle was promoted
from this experiment, and the shipped executable AOT/native-hook path is
unchanged.

## 2026-08-29 12:22 IDT: absolute function-pointer entry discovery

Extended `x86toc.py` to promote a target only when a direct `call
[absolute_slot]` reads an in-range, already decoded instruction address. With
the two previously explicit profiler-proven entries retained, the table is
`170676/170699` blocks (only `int3` and `ud2` remain unhandled). The dirty Wine
tree remains on branch `vibe` at commit
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; unrelated dirty/untracked work was
preserved.

The served indirect-call candidate is `webwine-bw.js` SHA-256
`56270e2e35c119a905f68571e80a6d4d2ec70ca0cf451d4c66974646c4ea135a`,
`webwine-bw.wasm` SHA-256
`60b779211f8f5cc3d7bee99c4938710395f99e2c4432951773b7bb3f6ae73de8`, and
`webwine-bw.data` SHA-256
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, served
at `http://localhost:8800/?WASM_DIAG=1`. Node differential verification
completed without `JITBAD`, `JITBADEIP`, `UNIMPLEMENTED`, or unknown-native
output. In a matched active Node interval it measured `77.0%` JIT coverage and
`132` interpreted kinsn/frame versus `75.7%` and `134` for the true no-entry
control; MIPS was within noise. The browser candidate reached first frame at
10.5s, reported `frames=74` by 15s on the 640x400 worker canvas, accepted
Space, logged `wasm_input: input target window 0x20028`, and produced nine
non-black samples with distinct screenshot hashes `fa7eff88` (10s) and
`bf360313` (15s). It remained alive through 18s.

Observation: the candidate is stable and reduces interpreted instruction work
slightly in the matched Node interval. Hypothesis: this is a small path-local
gain, not yet a statistically established end-to-end FPS increase; longer
steady-state A/B remains the next measurement.

## 2026-08-29 11:50 IDT: explicit profiler-proven entries candidate

The translator now accepts explicit `--entries=` addresses and freshly
disassembles only those entries when the linear scan crossed an undecodable
island. The candidate adds the profiler-observed executable entries
`0x00801232` and `0x00801310`, producing `170643/170666` translated blocks
(up from `170564/170587`). The raw-byte prologue scan remains removed. The
Wine tree is dirty on branch `vibe` at commit
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; unrelated dirty/untracked work was
preserved.

Candidate artifacts were `webwine-bw.js` SHA-256
`78d02a214c81b058f546eb211e40e5caa52cf95c8234c802a93d78802f184533`,
`webwine-bw.wasm` SHA-256
`23b72e815dc21c0aec23aaa9ef07da38ca2b2ac54de2fe5e50cf15c734b3cc6d`, and
`webwine-bw.data` SHA-256
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, served
at `http://localhost:8801/?WASM_DIAG=1&WASM_TPUT=1`. Node differential
verification completed with no `JITBAD`, `JITBADEIP`, `UNIMPLEMENTED`, or
unknown-native output. The 22-second browser run reached first frame at 9.9s,
reported `frames=71` by 15s on the 640x400 worker canvas, accepted Space, and
logged `wasm_input: input target window 0x20028`; it remained alive through
22s. Page-side pixel readback is unavailable on this worker OffscreenCanvas
path (`non-black=0`, `distinct frames=0` in the probe), so the canvas result is
worker frame status rather than screenshot proof.

Observation: the explicit-entry candidate is regression-free in this run.
Hypothesis: it may reduce misses only in execution paths reaching these two
entries; a matched steady-state A/B is still required before promotion.

Follow-up screenshot run against the same bundle at
`http://localhost:8800/?WASM_DIAG=1` reached first frame at 7.4s and
`frames=185` by 15s. It produced six non-black samples and distinct captured
frames, including screenshot hashes `78bdd8b4` at 10s and `bf360313` at 15s;
Space input again reached the guest (`wasm_input: input target window
0x20028`). This strengthens the rendering/stability evidence but still does
not constitute a clean performance A/B.

## 2026-08-28 02:04 IDT: raw-entry experiment reverted after browser OOB

The raw-byte prologue scan was tested as a material performance experiment,
then removed after it caused a correctness regression. The experimental table
had `170745/170768` blocks and recovered `gblk_008012e0`; its browser run
reached `frames=163` and first frame at 7.1s, then ended with
`FATAL worker exception: memory access out of bounds`. The source remains dirty
on branch `vibe` at commit
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; unrelated dirty/untracked work
was preserved. No commit was made for the experiment.

The reverted, regression-control browser artifact is now
`webwine-bw.js` SHA-256
`ca3cedebca731df15d38cb5a4ffd69e8a50990717edf4bbc554c43c880803d57`,
`webwine-bw.wasm` SHA-256
`d43d04375b47bea226c45c67bacd225696fefa01cd25d7d8ae6fe08c1101fd93`, and
`webwine-bw.data` SHA-256
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, served
at `http://localhost:8800/?WASM_DIAG=1`. The 16-second control run reached
first frame at 7.1s and `frames=164` (about 29.5 FPS at the 10-second probe);
Space was sent and the guest logged `wasm_input: input target window
0x20028`. The worker stayed alive through the end of the run. The probe's
page-side screenshot/WebGL counters were zero because rendering is on the
worker OffscreenCanvas, so this run records canvas dimensions and worker frame
status, not pixel readback. The expected headless Pointer Lock
`WrongDocumentError` remains non-gameplay input noise.

Observation: the raw-entry expansion is not safe to ship. Hypothesis: at least
one raw signature was data or an entry with an incompatible boundary/context;
this needs independent indirect-call proof before another attempt.

## 2026-08-28 01:40 IDT: indirect-function AOT coverage expansion

Added conservative function-prologue discovery to `x86toc.py`, recovering
indirectly reached executable helpers after callee-save push sequences, and
translated the remaining `pextrw` form. The generated table is now
`170564/170587` blocks with only `int3` and `ud2` reported unhandled. The
dirty Wine tree remains on `vibe` at commit
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`; unrelated dirty/untracked work
was preserved.

The promoted browser artifact is `webwine-bw.wasm` SHA-256
`b0c798b43e46917b4af354b9f1a0af5c08c2eaba7e90dcaea16c66e17a9aaaf3` with the
same data package SHA-256
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, served
at `http://localhost:8800/?WASM_DIAG=1`. `WASM_JIT_VERIFY=1` completed with
no `JITBAD`, `JITBADEIP`, `UNIMPLEMENTED`, or unknown-native output. A
16-second browser run reached first frame at 7.1s and `frames=167` on the
640x400 canvas; Space input was sent and the guest logged
`wasm_input: input target window 0x20028`. Headless Pointer Lock remains
unavailable. This verifies the expanded build, but does not yet establish a
statistically clean steady-state FPS uplift because warm-up and host load
dominate these short runs.

## 2026-08-28 01:29 IDT: post-prologue AOT build regression

The static translator now adds conservative function-prologue leaders for
indirectly reached helpers. It generated `170564/170587` blocks (the prior
build generated `165771/165794`); the dirty source remains on `vibe` at
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b`, with unrelated dirty/untracked
work preserved.

Served artifacts were `webwine-bw.wasm` SHA-256
`aaaf8f66d8a46199baa9c94b2ef5b0634c56d541550260fb6b1a4c24f4bf40f8` and
`webwine-bw.data` SHA-256
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, at
`http://localhost:8800/?WASM_DIAG=1` with COOP/COEP headers. Node
`WASM_JIT_VERIFY=1` completed with no `JITBAD`, `JITBADEIP`,
`UNIMPLEMENTED`, or unknown-native output. The browser run reached a real
640x400 frame at 13.5s, then `frames=59`, `non-black=10`, and two distinct
screenshot hashes (`84046bec` at 10s and `bf360313` at 16s); Space was sent
and the guest logged `wasm_input: input target window 0x20028`. Headless
Pointer Lock remains unavailable. This proves regression-free rendering and
input, but the short run does not establish a sustained FPS improvement; the
next measurement must isolate warm-up from steady state.

## 2026-08-28 01:02 IDT: browser bundle bootstrap check — no frame yet

Material test performed against the custom bundle URL
`http://localhost:8800/?WASM_DIAG=1` (not the documented `serve_gl.mjs`
showcase URL). The server was serving the dirty Wine tree at commit
`ad816e0a7dd68ee9030b0e8d89967e82a79f325b` (`vibe`); source changes were
uncommitted and unrelated dirty/untracked files were preserved.

Artifacts served:

- `webwine-bw.wasm`: SHA-256
  `83b375e19825a81d9daf65cc4329c49e10845a3dfe46bf3407eaa038d485ff99`
- `webwine-bw.data`: SHA-256
  `d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`

The initial 75-second CDP run appeared to show no frames because its probes
only watched the page's WebGL context; rendering actually occurs on the
worker's `OffscreenCanvas`. An instrumented follow-up reported `frames=65` and
first frame at 7.7s. Screenshot-backed proof at 01:03 IDT reached
`frames=219`, `non-black=8`, and `distinct frames=2`; screenshots at 10s and
15s had hashes `84046bec` and `bf360313` (the latter 9,746 bytes). Canvas was
640x400 throughout. The synthetic Space input was sent (`keys 2`, `mouse 2`)
and the guest logged `wasm_input: input target window 0x20028`; headless Chrome
cannot acquire Pointer Lock, so mouselook is not proven by this run. The
bundle is therefore rendering and accepting ordinary keyboard input; the
page-side WebGL counters remain `upload=0 draw=0` by design for this worker
path.

## 2026-08-19 (evening): DYNAMIC-CODE AMNESTY — ~132 → 205 fps mean, peaks at the 250 cap

The interpreted-renderer ceiling is broken. The mechanism ("dynamic-code
amnesty", all in the sibling BoxedWine tree, ST WASM-JIT builds only):

- **Root cause recap, now exact:** just **2 poisoned bytes** (write counts
  hitting `MAX_DYNAMIC_COUNT`=255 at guest `0x631DB4`/`0x63246F`) marked the
  Build-engine renderer's inner-loop ops `OP_FLAG_NO_JIT`, fragmenting the loop
  into compiled islands around permanently-interpreted ops — 78.7% of hot block
  exits landed on non-compiled targets (BWDIAG3 attribution: 99.6% of those were
  NO_JIT; pending/warming/Done were noise).
- **The amnesty** (`codePageData collectStaleDynamicPages` + registry sweep in
  `kmemory.cpp` + a ~2s tick in the emscripten mainloop): un-poison poisoned
  pages on a cooldown — invalidate their decoded ops (they re-decode
  JIT-eligible) and clear the write counts. Correctness is untouched: every
  code write still invalidates overlapping blocks; only the "never compile
  again" verdict is reversed. The 255-writes-per-byte threshold itself brakes
  churn — a page must absorb 255 writes to a byte between amnesties, so even
  per-frame SMC costs at most one page invalidation + recompile per tick.
  Re-poisoned pages are fast-tracked to the next tick (≤2s interpreted window).
- **Gotchas that cost hours:** (1) `KThread::currentThread()` at the mainloop
  tick usually is NOT the game process — the sweep must iterate a registry of
  live `KMemory` instances (ctor/dtor registered); (2) a process's KMemory can
  outlive its `data` (exit path nulls it) — null-guard or the sweep traps.
- **Measured (clean 220 s run, `up3m250s`):** last-120s **mean 205.6 fps, min
  148, max 250** (the game's own `r_maxfps 250` is now the binding cap; guest
  MIPS peaks ~505). Frame pacing flawless: p50 4.1 ms / p95 6.0 / p99 7.5 /
  max 19 ms, **zero frames >50 ms**. Screenshot-verified correct rendering.
- Also in this round: `NormalCPU::run()`'s per-dispatch decoded-op refetch is
  now epoch-guarded (`g_bwDecodedOpCacheEpoch`, bumped on any op removal) —
  removals are <200/s while dispatches are millions/s, so the per-block cache
  lookup disappears at steady state.

**Uncommitted-work incident (cautionary):** a `git checkout --` during
diagnostics destroyed uncommitted BoxedWine deltas. Forensics (binary name/type
diffs vs the shipped wasm) showed the losses were the `EM_TIMING_SETIMMEDIATE`
mainloop fix and the paced-flush scheme (both restored from their documented
specs: `queueRuntimeFlushes` compiles at most one batch per call +
`wasmJitDrainSealedRequests()` drains one per browser tick), plus a
`loadAllGPRegsForExit` helper of unknown provenance (not restored; the
committed tree is self-consistent without it). The reg-param-ABI/TLB-caching
declarations in the dirty `jitWasmCodeGen.h` are unimplemented WIP, not lost
features. **The BoxedWine tree should be committed** to prevent a repeat.

The old 132-fps binaries are kept as `build-jitgl/boxedwine.{wasm,js}.prev-132fps`.

**Shipped + soak-validated:** the deployed build adds erase-on-collect (an
amnestied page that never re-poisons is never touched again), a null-thread
guard on the sweep, and full `disableWasmJitForWrittenCode` opt-out. A
10.5-minute soak under external CPU contention: fps steady (no degradation,
145 mean contended ≈ 2x the old build under equal load), **module memory flat
at 171 MB** across 294k cumulative block compiles (the ~2s amnesty/re-poison
cycle recompiles ~380 blocks per round and the freed ones genuinely release —
no leak), 308 amnesty cycles without incident. BoxedWine work is committed
locally (`dd4e243`, `f87f5c0` on its master). Mobile (4x-throttle) validation
is pending a quiet machine — the native `vibebuild32` test ran all evening;
expectation is a large win since the interpreted renderer was exactly what a
throttled device could not afford.

## 2026-08-19 (later): fps-ceiling root cause + gzip transfer win

**Why netduke32 tops out at ~132 fps — definitively diagnosed.** Fully-warm V8
CPU profile (t≈190–220 s) is ~**67% interpreter-related** (ops + `normalDispatch`
+ `NormalCPU::run` + `getOp`) and only ~**18% compiled** JIT code. Using
BoxedWine's built-in fetch-next transition recorder (`jit-record=true` →
`[WASM JIT transitions]` klog) plus custom instrumentation, the cause is exact:
**72% of hot-block exits go to a non-JIT-compiled target, and 99% of those are
`OP_FLAG_NO_JIT` (dynamic-marked).** One page dominates — guest `0x631000`
(module "Unknown" = anonymous, i.e. the Build-engine software renderer) at
**312.7 M of 315.9 M noFlag hits (99%)**. That page is marked dynamic (a byte hit
`MAX_DYNAMIC_COUNT`=255 writes) so `firstDynamicOp` never compiles it; it runs
interpreted forever. The JIT itself is healthy: it converges to ~**49 000 blocks**
(a finite working set, 240 MB of modules) and fps plateaus by ~**t=85 s** — the
blocks compiled after that are cold and don't move fps. So the ceiling is the
interpreted renderer, not warmup, not churn (invalidations <167/s), not poisoning
volume (<100 bytes ever hit 255 — but a few in the renderer page are enough).

**Tried and rejected: a "dynamic-code amnesty"** (periodically re-decode
dynamic-marked pages not written recently, so a renderer patched once at
video-mode setup could become JIT-eligible; one-shot per page to avoid churn).
Built and A/B'd (`build-jitgl` vs the amnesty build). In clean windows it ran
**on par with production (~127 vs ~132), no reliable win** — the renderer is
evidently *genuinely* self-modified (per-frame patching), so it is correctly
`NO_JIT` and cannot be compiled without a JIT that reads the patched immediates
from memory instead of baking them (a large project). Reverted; the sibling
BoxedWine tree is clean. Measurement was hampered throughout by the user's native
`vibebuild32` intermittently taking a core — trust only same-load A/B, per the
long-standing discipline note.

**Shipped: HTTP gzip on the dev servers** (`serve.mjs` + `serve-https.mjs`).
The big compressible assets now transfer far smaller — **boxedwine.wasm
5.7 MB→1.2 MB (4.8×)** and **netduke32-up3m250s.zip 66 MB→29 MB (2.3×)** — while
the already-packed root zip (195 MB, ~2% gain) is streamed raw (each file's
worth-it decision is cached; skip if it doesn't shrink to <90%). gzip is
transparent to the browser and to BoxedWine's fetch, and the stored (deflate-0)
zips still avoid runtime re-inflate, so this is a pure load-time win — most
valuable for the phone-on-LAN https flow. Verified end-to-end: the game boots and
renders in-level through the gzip server.

## 2026-08-19: mobile performance envelope + factor-4 variant

Added CPU-throttle emulation (`--throttle N`) and frame-interval/jitter metrics
(`frame-intervals.json`, p50/p95/p99 + hitch %) to cdp-run so mobile behaviour
can be measured on the desktop. A mid phone is ~4x slower than this M5, a
high-end phone ~2x. Clean-machine numbers (steady state, after warmup):

| device (throttle) | factor 3 (up3m250s) | factor 4 (mob4) |
| --- | --- | --- |
| desktop (1x)  | ~115 fps | ~55–90 fps, p99 29 ms |
| high-end (2x) | — | ~28 fps, 1.7% frames >100 ms |
| mid (4x)      | 19.5 fps | **23 fps, min 20** |
| low (6x)      | 5.5 fps  | 6.6 fps (unplayable) |

**`netduke32-mob4.zip`** (`r_upscalefactor 4` = 160x100 internal, else same as
up3m250s) is the mobile recommendation: +19% fps and tighter frame times than
factor 3, at the cost of a chunkier 3D view. Serve build-jitgl; same URL params
with `app=netduke32-mob4.zip`.

**All hitches are JIT warmup** in the first ~15–20 s (compile burst); after that
zero stalls >200 ms for minutes. Two mobile-smoothness attempts that FAILED and
were reverted: (1) rate-limiting JIT compile to a duty cycle — starves warmup on
slow devices (guest runs interpreted → 1.6 fps at 4x, a death spiral; fast
warmup wins on mobile even with some early hitches); (2) an `r_maxfps 30` cap —
no benefit, the device runs below the cap so it never binds. Real remaining
levers need engine work: background-thread JIT compile (the MT build doesn't
launch the guest), higher JIT block coverage, or the persisted cache without its
re-install pathology. iOS caveats (unverified): Safari has **no Pointer Lock**
(mouselook won't engage) and strict per-tab memory limits vs the JIT's ~290 MB
of modules — a crash risk to watch.

Measurement note: throttle runs are extremely sensitive to other CPU load
(a second Chrome or a native build tanks them to near-zero) — only trust
throttled numbers on a quiet machine, or use back-to-back A/B under equal load.

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

## Browser black-screen follow-up (2026-08-31 14:54 IDT)

Observation: the served browser page was receiving frames, but the first
startup/transition frames were nearly black. The page previously hid its boot
notice on any first frame, making that valid startup interval look like a dead
black screen. The browser UI now keeps the notice until sampled canvas
brightness is meaningful. This is a Wine-repository browser UI change; source
is dirty and contains prior uncommitted work.

Test URL: `http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`

Served artifact SHA-256 values:

```text
webwine-bw.js    f94a198753bcfbb5883ac7898c2c2a1f25cfd50ae05862568757793de76c3359
webwine-bw.wasm  4928baaeb1f4392fdb000ee6d8cf9c58655a5918571175e6b7a6de102a02c5c9
webwine-bw.data  d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb
worker.js        72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651
index.html       6a8c7cf908d5296f2077dbe26cf0c0eec852ec4ef774db95fc77848993ba01c4
```

Decisive lines from the fresh run: `wasm_x86: JIT 170725 translated blocks
loaded`, `GFX| OpenGL context: version 3.3`, and `input: ready`. At 6 seconds
the canvas was still 640x400 and the visible `Starting Wine…` overlay remained
over the nearly-black startup frame. In the corresponding run at 12 seconds,
the 640x400 canvas contained the non-black 3D Realms startup logo and the
overlay was gone. No `FATAL`, `RuntimeError`, `UNIMPLEMENTED`, `JITBAD`, or
`JITBADEIP` appeared. This confirms the reported black screen was startup
presentation timing/UI ambiguity, not a worker death in this build.

Hypothesis: on slower devices the initial interpreter/JIT warm-up can exceed
the old “usually 5–10 seconds” estimate; the overlay now communicates that
state instead of presenting an unexplained black canvas. Sustained rendering
and input remain covered by the prior full-run evidence, not by the short UI
regression run.

## Runtime thunk-tail candidate (2026-08-31 15:24 IDT)

Observation: the Wine-only `5d ff 25` generated-thunk tail fast path is now
served at the canonical URL. The exact 30-second run produced 30 canvas
samples, 20 non-black samples, 13 distinct frames, a 640x400 canvas, and
`input: ready`; the final sample reported `frames: 578`, first frame at 8.2s,
and `JITCOV ... jit_frac=98.2%`. No `FATAL`, `RuntimeError`, `UNIMPLEMENTED`,
`JITBAD`, or `JITBADEIP` appeared. Warm samples were approximately 1,000 FPS
with normal transition dips, matching the existing control range rather than
demonstrating a measurable gain yet.

Test URL: `http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`

Current served hashes: JS `f94a198753bcfbb5883ac7898c2c2a1f25cfd50ae05862568757793de76c3359`,
WASM `ea2bf8cf4ba7aff71dea333944d1f2dbce1db4e2cd40a46ce390c465392c341c`,
DATA `d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`,
worker `72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`,
index `6a8c7cf908d5296f2077dbe26cf0c0eec852ec4ef774db95fc77848993ba01c4`.
The source tree remains dirty with prior uncommitted work; no sibling checkout
was modified.

Hypothesis: the tail path is correct but too small or too infrequent to move
the steady-state FPS on this workload. Keep it as a correctness-clean
candidate while the next effort targets larger runtime-generated blocks.

The follow-up runtime-byte probe at the same URL (2026-08-31 15:25 IDT)
confirmed the bundle still reached changing 640x400 frames and `input: ready`
without fatal/JIT errors, but did not capture a stable `3de31d2c` byte dump
before that transient generated allocation moved on. The runtime-generated
family therefore remains an observation target, not a guessed optimization.
# 2026-08-31 22:27 IDT: reverted unverified default x87 shortcut; traced black-looking stall

The canonical URL tested was
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`. The Wine source tree is
dirty on branch `vibe` at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`; unrelated
changes were preserved. The served bundle was rebuilt from that tree. Artifact
SHA-256 values are JS
`b5ccbe12c0672697bea8b5c8963aa7a0dcab841da23d9df16a18bc91ca92ff1b`, WASM
`1693623fde1f2410133a270f90d6e2980ddcd4e18d25affe101b1adca43f2119`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index `54d81676870b356aa800b9dd44bf8c27276f886edf8bb4bb8da80f85dafdf571`.

Observation: `_pow` is now opt-in via `WASM_EXPERIMENT_POW`; the default
bundle logs `native setupqrhlineasm4` and `native qrhlineasm4`. A 21-second
canonical CDP run reached a non-black 320×200 canvas, screenshot hash
`e2251540`, and 773 frames by 15.7 seconds. It then spent several seconds in
a large software-render frame and resumed at 22.8 seconds with 785 frames;
there was no `FATAL`, `JITBAD`, `JITBADEIP`, or `RuntimeError`.

Observation: disabling qrhline native caused the stall trace to report
`STALLBT flips=428 eip=00633386`, inside the qrhline body. Disabling the other
native mapper hooks did not remove the long render interval. This is a slow
render interval that can look like a black/stuck page, not a failed GL context
or dead worker.

Hypothesis: the remaining user-visible pause is the unhandled/slow software
render path reached after the intro; the next fix needs to make that path
incremental or translate it safely. The default x87 `pow()` substitution was
not sufficiently proven and remains disabled to avoid reintroducing a true
renderer regression.
# 2026-08-31 22:47 IDT: native pow promoted after profile

The canonical URL tested was
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`. The Wine tree remains dirty
on branch `vibe` at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`; unrelated work
was preserved. The rebuilt served artifacts are JS
`b5ccbe12c0672697bea8b5c8963aa7a0dcab841da23d9df16a18bc91ca92ff1b`, WASM
`067c1ea7e8088b49342dcff33a733fba0fb2a503b4bf00f304908387d53cbeaa`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index `54d81676870b356aa800b9dd44bf8c27276f886edf8bb4bb8da80f85dafdf571`.

Observation: the run logged `native pow (x87 return) @ 007b8360`, reached a
real 320×200 frame with screenshot hash `e2251540`, and measured approximately
575–750 FPS through the first sustained samples. It had no `FATAL`, `JITBAD`,
`JITBADEIP`, or `RuntimeError`. A later heavy software-render interval reduced
the instantaneous sample to zero temporarily; the worker remained alive.

Change: the pow hook is now default-enabled with `WASM_NO_POW` as an explicit
rollback switch. Its skeleton remains exact-checked at startup, and it pushes
the host result onto the guest x87 stack while preserving the cdecl stack
contract.

Hypothesis: `_pow` was a material interpreted hotspot and this promotion is a
measured FPS improvement. The remaining long frame is a separate software
render path; its exact function boundary still needs a clean stall backtrace.
# 2026-08-31 22:46 IDT: post-pow profile moved residue out of the executable

The exact profile URL was
`http://localhost:8806/?WASM_TPUT=1&WASM_IPAGE=1`. The dirty Wine `vibe`
tree is at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`. Served artifact hashes
remain JS `b5ccbe12c0672697bea8b5c8963aa7a0dcab841da23d9df16a18bc91ca92ff1b`,
WASM `067c1ea7e8088b49342dcff33a733fba0fb2a503b4bf00f304908387d53cbeaa`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index `54d81676870b356aa800b9dd44bf8c27276f886edf8bb4bb8da80f85dafdf571`.

Observation: the profile logged the native pow hook and, after the first
frame, no longer had `007b0000` as the dominant executable interpreted page.
The warm sample reached `FPSSAMPLE t=18.6 flips=1049 fps=21.8` during a heavy
frame, while normal preceding samples were in the 600–750 FPS range. The
dominant remaining miss family was msvcrt/Wine helper code, led by
`3ee39b80` and `3ee3b5*`; no `FATAL`, `JITBAD`, `JITBADEIP`, or `RuntimeError`
was observed and the canvas screenshot remained non-black (`e2251540`).

Hypothesis: the native pow change is effective; the next optimization should
be a complete function-level reduction of the msvcrt helper family, not more
guest mapper hooks. The profile is cumulative outside the frame-scoped mode,
so its early-startup counts are not sufficient to promote an interior helper
shortcut without another frame-scoped run.
# 2026-08-31 23:27 IDT: generalized inthash fast path verified in browser

The canonical URL was
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`; the dirty `vibe` tree is at
`ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`. Served hashes are JS
`b5ccbe12c0672697bea8b5c8963aa7a0dcab841da23d9df16a18bc91ca92ff1b`, WASM
`ae16316ee7a64d2d6fccacad90ec3b145675f5a8b1f49347bcce19bc6cf02063`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index `54d81676870b356aa800b9dd44bf8c27276f886edf8bb4bb8da80f85dafdf571`.

Observation: `nat_inthash_find` now mirrors the verified x86 djb2 hash,
multiply-high bucket reduction, and three-word chain lookup for general tables;
the GL no-state shortcut and malformed/uninitialized fallback remain intact.
The enabled run reached 3,700+ frames with warm samples around 700–780 FPS,
320×200 non-black output, and no `FATAL`, `JITBAD`, `JITBADEIP`, or
`RuntimeError`. The explicit rollback run with `WASM_NO_INTHASH_FAST=1` also
rendered successfully, reaching 4,700+ frames and warm samples around
960–978 FPS before the same heavy render interval.

Hypothesis: the fast path removes a real interpreted function family, but the
short A/B is timing-sensitive and does not yet prove a net FPS gain. The next
measurement should use matched frame-scoped runs; no broader inthash interior
shortcut should be added without that comparison.
# 2026-09-01 16:25 IDT: rebuilt browser bundle and verified black-screen report

The canonical URL tested was
`http://localhost:8806/?WASM_TPUT=1&WASM_BADIP=1`. The Wine tree remains
dirty on branch `vibe` at `ad816e0a7dd68ee903b0b0e8d89967e82a79f325b`; no
sibling checkout was modified. The bundle was rebuilt from the current tree.
Artifact SHA-256 values are JS
`d3e1cb115819a976e4cf77c2a57b9448215cde931e64e0f649059131b698ab02`, WASM
`eb52fcdef2f4b5443b6086e195366243f9ee376613e9586be1e104879bb8503e`, data
`d88e01f461b152239b3e434a5582f4be813b248a5c882f0e41d383bf965131bb`, worker
`72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`, and
index `54d81676870b356aa800b9dd44bf8c27276f886edf8bb4bb8da80f85dafdf571`.

Observation: a fresh Chrome run initially displayed the loading overlay, then
logged `GFX| OpenGL driver: WebKit WebGL 3.3`, `GFX| OpenGL context: version
3.3`, and `Executing autoexec.cfg`; it presented a real non-black 320x200
frame at approximately 6.5 seconds. The screenshot hash was `e2251540`, input
reported `input: ready`, and no `FATAL`, `RuntimeError`, `JITBAD`, or
`JITBADEIP` appeared. The same result was reproduced on port 8799.

Observation: after roughly 11–12 seconds a heavy software-render interval can
temporarily drive the displayed FPS to zero while retaining the last frame;
this is a performance stall, not a black framebuffer or dead worker.

Hypothesis: a user seeing only black is most likely viewing a stale/cached
worker or the initial boot canvas. The page now cache-busts the worker, WASM,
and data requests and shows an explicit boot/worker-failure state; use the
exact URL above and hard-refresh once.

# 2026-09-04 12:15 IDT: rejected forced-inline FPS experiment

The candidate was tested at
`http://localhost:8807/?WASM_TPUT=1&WW_ARGS=%2Fv1,%2Fl1&build=inline-20260904d`
with the repository COOP/COEP server. The unchanged control was tested at
`http://localhost:8808/?WASM_TPUT=1&WW_ARGS=%2Fv1,%2Fl1&build=baseline-20260904a`.
The Wine tree is dirty on branch `vibe` at `9a9615fd`; unrelated generated and
platform artifacts were preserved. The candidate artifact hashes were JS
`ec9fc0864c8de9f8242b84a84d2f927c7d3b4778ebfd001ae754afc68ee1a1f3`, WASM
`d45a4e25591d879554d1fc22cac6eb9d087250efe722f6936416bd1e9a2afeb0`, data
`b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`, index
`455e20ff86b48a6c3e880dd5558bc54c2f749845b2fee6ee7fa343407bd9bcc6`, and
worker `72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`.
The published control remains JS
`ec9fc0864c8de9f8242b84a84d2f927c7d3b4778ebfd001ae754afc68ee1a1f3`, WASM
`fe9b426a0ee8001edc831d0189fc56d3dd306977bbb96c390603bf7d2e0ed625`, with
the same data, index, and worker hashes above.

Observation: both runs reached `E1L1: HOLLYWOOD HOLOCAUST`, presented a real
640×400 frame with screenshot hash `a227e01c`, reported `input: ready`, and
had no `JITBAD`, `FATAL`, `RuntimeError`, or assertion marker. The candidate
reported `FPSSAMPLE t=20.8 ... flips=831 ... fps=94.4`; the matched control
reported `FPSSAMPLE t=20.9 ... flips=881 ... fps=96.6`. The candidate therefore
lost about 6% on this controlled run and was reverted; it is not promoted.

Hypothesis: forcing `set_lazy`, `read_reg`, and `write_reg` inline increases
code pressure or register pressure in the hot translated path. No further
compiler-only promotion should be made without a matched multi-run gain.

# 2026-09-04 12:21 IDT: rejected 64-bit MSVCRT memset loop experiment

The candidate was tested at
`http://localhost:8807/?WASM_TPUT=1&WW_ARGS=%2Fv1,%2Fl1&build=memset64-20260904a`
with COOP/COEP and the unchanged control at
`http://localhost:8808/?WASM_TPUT=1&WW_ARGS=%2Fv1,%2Fl1&build=baseline-20260904a`.
The Wine tree is dirty on branch `vibe`; generated and unrelated platform
artifacts were preserved. The candidate JS/WASM hashes were
`ec9fc0864c8de9f8242b84a84d2f927c7d3b4778ebfd001ae754afc68ee1a1f3` and
`057d98699ac7e852a2dd716b00ab6856c4ff1a133f40a744937ddbf1093de85d`;
data/index/worker remained the published hashes recorded above.

Observation: the candidate reached `E1L1: HOLLYWOOD HOLOCAUST`, presented
the real 640×400 frame with hash `a227e01c`, reported `input: ready`, and had
no `JITBAD`, `FATAL`, `RuntimeError`, or assertion marker. Its warm samples
were approximately 60–68 FPS (`FPSSAMPLE t=20.0 ... flips=453 ... fps=60.6`),
whereas the matched control reached approximately 83–99 FPS
(`t=20.9 ... flips=881 ... fps=96.6`). The candidate was reverted and not
published.

Hypothesis: the explicit 32-bit stores are already a better fit for this
WebAssembly memory path, or the memset loop is not frame-dominant. The next
FPS change must come from a frame-scoped executable/Wine hotspot rather than
another store-width rewrite.

# 2026-09-04 12:32 IDT: frame-scoped hotspot re-profiled

The canonical diagnostic URL was
`http://localhost:8799/?WASM_TPUT=1&WASM_IPAGE=1&WASM_IPAGE_FRAME=1&WW_ARGS=%2Fv1,%2Fl1&build=frameprofile-20260904a`.
The Wine tree is dirty only from preserved untracked build/cache artifacts on
branch `vibe`; tracked source is clean at `483d4bf4`, and no sibling checkout
was modified. The canonical artifact hashes remain JS
`ec9fc0864c8de9f8242b84a84d2f927c7d3b4778ebfd001ae754afc68ee1a1f3`, WASM
`fe9b426a0ee8001edc831d0189fc56d3dd306977bbb96c390603bf7d2e0ed625`, data
`b6e7c288b2cc5f9e5a83a153561d4d385f8eb073e538258ac7ebf65d947e4b63`, index
`455e20ff86b48a6c3e880dd5558bc54c2f749845b2fee6ee7fa343407bd9bcc6`, and
worker `72605037636d97a478c14e43b9f614f8d4aeb270769a94a9598b04c85c249651`.

Observation: after frame-scoped counters were enabled, the largest remaining
misses were executable renderer/SSE boundaries, led by `0055b6f0/0055b6f8`,
`00529500`, `00555610`, and `005599c0`, each roughly 0.1M in this run. The
run still reached `E1L1: HOLLYWOOD HOLOCAUST`, presented a non-black 640×400
canvas, reported `input: ready`, and showed no `JITBAD`, `FATAL`,
`RuntimeError`, or assertion marker. Diagnostic overhead made its FPS samples
non-comparable and they are intentionally not used as a performance claim.

Decision: the next candidate should be a verified function-level renderer/SSE
translation or native hook for one complete call boundary, with a matched
non-diagnostic control. Broadly seeding individual interior addresses has
already been rejected.

# 2026-09-04 12:38 IDT: rejected isolated polymost_drawpoly entry seed

The candidate added only the complete executable entry
`0x0055b6f0` (`polymost_drawpoly`) to the generated AOT seed list and was
tested at
`http://localhost:8807/?WASM_TPUT=1&WW_ARGS=%2Fv1,%2Fl1&build=entry55b6f0-20260904a`.
The stable control was previously measured through the identical COOP/COEP
harness at port 8808. The candidate WASM hash was
`74db770797655de0c6a7e77f46384048707672e9a571c5dbb7e3ca17e0a43bda`; the
canonical published WASM remains
`fe9b426a0ee8001edc831d0189fc56d3dd306977bbb96c390603bf7d2e0ed625`.
The Wine tree is dirty only from preserved untracked build/cache artifacts;
the generated table was restored and no sibling checkout was modified.

Observation: the candidate generated 170,758 translated blocks, reached
`E1L1: HOLLYWOOD HOLOCAUST`, and presented a real 640×400 frame. It did not
produce `JITBAD`, `FATAL`, `RuntimeError`, or assertion output, but reached
only `FPSSAMPLE t=21.5 ... flips=64 ... fps=30.8`; the matched stable control
reached `t=20.9 ... flips=881 ... fps=96.6`. The seed was reverted and is not
published.

Hypothesis: this function’s prologue/stack-alignment entry is called often
enough that its isolated translation adds dispatch/code-pressure cost, or its
indirect-entry assumptions are not profitable. Further renderer work needs
whole hot-loop/function profiling, not isolated AOT entry seeding.

# 2026-09-04 12:46 IDT: rejected whole-interpreter O3 build

The candidate was built with `XOPT=-O3` and tested at
`http://localhost:8807/?WASM_TPUT=1&WW_ARGS=%2Fv1,%2Fl1&build=xopt3-20260904a`
using the COOP/COEP server. Its WASM hash was
`bdc0b00ee9ac925bc1b4015f1869f3445d50a50f3430de7d18d0ab255315ecc1`.
The canonical control remains the published WASM
`fe9b426a0ee8001edc831d0189fc56d3dd306977bbb96c390603bf7d2e0ed625` and
the stable URL is
`http://localhost:8799/?WASM_TPUT=1&WW_ARGS=%2Fv1,%2Fl1&build=baseline-restored`.
The Wine tree is dirty only from preserved untracked build/cache artifacts;
tracked source and the generated AOT table were restored, and no sibling
checkout was modified.

Observation: the O3 run reported `input: ready` and no
`JITBAD`, `FATAL`, `RuntimeError`, or assertion, but it remained in startup
through the 20-second test and never logged `E1L1` or a real gameplay frame.
The stable O2 control reached E1L1 and the real 640×400 frame within the same
test budget. The candidate was discarded and not published.

Hypothesis: O3 increases code size and compile/runtime pressure in the very
large generated interpreter translation unit. Keep `XOPT=-O2` as the browser
baseline; future FPS work must target measured hot code rather than global
compiler aggressiveness.
