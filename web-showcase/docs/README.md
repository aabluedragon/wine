# Browser OpenGL handoff

This directory is the source of truth for continuing the VibeBuild32 browser
investigation from a new session.

Read in this order:

1. [current-state.md](current-state.md) — exact dated checkpoint, hashes,
   launch URL, last result, and immediate next action.
2. [reproduction.md](reproduction.md) — serve, rebuild, capture, and validation
   commands.
3. [wasm-opengl-blocker.md](wasm-opengl-blocker.md) — evidence, cleared
   blockers, current localization, and experiment history.
4. [wine11-driver-debugging.md](wine11-driver-debugging.md) — packaged Wine
   11.0 module provenance and the matched debug-rebuild procedure.

## Scope and branch

All work belongs on the repository's `vibe` branch. The goal is a browser-run
VibeBuild32/NetDuke-compatible PE32 using its 32-bit OpenGL mode through Wine
and BoxedWine/WebGL. The old 8-bit software presentation route is not an
acceptable substitute for closing this task.

The relevant sibling checkouts are:

- `/Users/alonamir/dev/wine` — this fork, branch `vibe`.
- `/Users/alonamir/dev/boxedwine` — emulator and WebGL bridge; intentionally
  dirty.
- `/Users/alonamir/dev/eduke32` — VibeBuild32 source and packaged PE32 input.
- `/Users/alonamir/dev/emsdk` — Emscripten SDK.

## Five-minute resume checklist

```sh
cd /Users/alonamir/dev/wine
git branch --show-current
git status --short
git -C /Users/alonamir/dev/boxedwine status --short
shasum -a 256 \
  web-showcase/build-gl/boxedwine.js \
  web-showcase/build-gl/boxedwine.wasm \
  web-showcase/build-gl/tinycore-wine11-parent-inline-webgl-pci-glxshim.zip \
  web-showcase/build-gl/tinycore-wine11-parent-inline-webgl-pci-glxshim-gl2ext.zip \
  web-showcase/build-gl/netduke32.zip \
  web-showcase/tools/patch-wine11-gl2-extension-count.mjs
lsof -nP -iTCP:8082 -iTCP:9555 -sTCP:LISTEN
```

Expected branch: `vibe`. Compare hashes with `current-state.md`. Artifact names
alone are not authoritative; several experimental ZIPs have similar names,
and `build-gl/tinycore-wine11.zip` is a symlink to the unmodified base root.

Before editing, inspect the focused state in all three working trees:

```sh
git diff -- web-showcase dlls/winex11.drv/x11drv_main.c
git -C /Users/alonamir/dev/boxedwine diff -- \
  source/opengl/sdl/sdlgl.cpp source/opengl/glcommon.cpp
git -C /Users/alonamir/dev/eduke32 status --short
git -C /Users/alonamir/dev/eduke32 diff -- \
  source/imgui/src/imgui_impl_opengl3.cpp GNUmakefile
```

## Truthful checkpoint

At handoff, the old `wglMakeCurrent returned 0xc0000005` stale-context crash is
fixed. The original GLX-shim root still reproduces the later NULL
`glGetStringi` regression. The newer `gl2ext` derivative proves that Wine
11.0 had synthesized a positive extension count before guest-driver dispatch;
falling through to BoxedWine produces the expected zero and clears that crash.
The latest run reaches `glShadeModel`, then throws browser-side
`RuntimeError: null function`. WASM disassembly and resolver tracing prove the
NULL target is BoxedWine `pglShadeModel`; the WebGL-compatible fallback is
documented but not yet applied. The newer client-state diagnostic reaches a
non-black startup frame but tears the context down, so it is not rendering
success. Read the exact artifact comparison in [current-state.md](current-state.md)
before making another change.

Scope rule: only this Wine repository may be edited. Never edit, build, clean,
reset, stash, or otherwise experiment with `/Users/alonamir/dev/eduke32`,
`/Users/alonamir/dev/boxedwine`, or another sibling source tree unless the user
specifically authorizes that named action. The executable under test is the
user-supplied `/Users/alonamir/games/netduke32_v1.2.1/netduke32.exe`, not a
rebuild from a sibling source checkout.

After every material test, update `current-state.md` rather than rewriting
history in the blocker report. Record both failures and successes, including
the artifact hashes and whether the canvas contained a real frame.
