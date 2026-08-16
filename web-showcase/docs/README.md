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

Before editing, inspect only this repository's focused state:

```sh
git diff -- web-showcase dlls/winex11.drv/x11drv_main.c
```

Do not replace this with `git -C` commands that build or modify the sibling
trees. Their provenance is already recorded in the checkpoint; this session's
authorized work area is the Wine repository only.

## Truthful checkpoint

At handoff, the stale-context crash, NULL `glGetStringi`, and optional sampler/
sync NULL calls are cleared in Wine-only diagnostic derivatives. The current
slotshim4 run still reaches a NULL `glad_glVertexPointer`/host attribute-pointer
boundary after buffer setup. The blurry low-resolution image is only a scaled
10x10/0x0 backing buffer, not gameplay; it later tears down to black. Read
[current-state.md](current-state.md) for exact hashes and scope.

Only `/Users/alonamir/dev/wine` may be edited. Never edit, build, clean, reset,
stash, or experiment with sibling `/Users/alonamir/dev/eduke32` or
`/Users/alonamir/dev/boxedwine` without explicit authorization. Run only the
user-supplied PE documented in the checkpoint.

Scope rule: only this Wine repository may be edited. Never edit, build, clean,
reset, stash, or otherwise experiment with `/Users/alonamir/dev/eduke32`,
`/Users/alonamir/dev/boxedwine`, or another sibling source tree unless the user
specifically authorizes that named action. The executable under test is the
user-supplied `/Users/alonamir/games/netduke32_v1.2.1/netduke32.exe`, not a
rebuild from a sibling source checkout.

After every material test, update `current-state.md` rather than rewriting
history in the blocker report. Record both failures and successes, including
the artifact hashes and whether the canvas contained a real frame.
