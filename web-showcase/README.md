# VibeBuild32 Web Showcase

This project packages the fork's VibeBuild32 v1.3 PE32 executable under the
name `netduke32.exe` and runs it through BoxedWine (Wine 11, a Linux syscall
layer, and an x86-to-WASM runtime). It is a NetDuke-compatible Windows build,
not a native web port.

## Current status

As last verified on 2026-08-17, the 32-bit OpenGL path is **not yet playable in
a browser**. Wine-only diagnostic derivatives clear the earlier context,
`glGetStringi`, sampler, and sync crashes. The current slotshim4 run reaches a
NULL `glad_glVertexPointer` call after buffer setup. The blurry low-resolution
image is a scaled 10x10/0x0 WebGL backing buffer, not gameplay; it later tears
down to black. Sustained rendering and input remain unproven.

The work is deliberately preserved for continuation. Start with the
[handoff index](docs/README.md) and its
[live checkpoint](docs/current-state.md) before changing the build.

## Build

```sh
make -C web-showcase        # tracked JIT recipe plus application overlay
make -C web-showcase serve  # serve build/ at http://localhost:8080/
```

This tracked recipe is not the current diagnostic configuration. The current
failure was reproduced with BoxedWine `Build/Control`, the `build-gl` output,
and an explicit GLX-shim Wine root. Use the
[reproduction runbook](docs/reproduction.md) for that configuration.

The build needs sibling checkouts of BoxedWine, EDuke32, and emsdk. Override
their locations if needed:

```sh
make -C web-showcase \
  BOXEDWINE=/path/to/boxedwine \
  EDUKE32=/path/to/eduke32 \
  EMSDK=/path/to/emsdk
```

`DUKE3D.GRP`, `eduke32.dat`, and related game files are copied from the local
EDuke32 checkout and intentionally remain untracked.

`make -C web-showcase app` copies `vibebuild32.exe`, renames it to
`netduke32.exe`, writes the 32-bpp configuration, and packages a `run.bat`
virtual-desktop launcher. The preserved diagnostic currently invokes
`netduke32.exe` directly with an explicit working directory and configuration
argument; do not assume the two launch routes are equivalent.

If changing the PE32 application, follow the
[dirty-tree build and A/B packaging procedure](docs/reproduction.md#optional-application-side-imgui-gl2-hardening).
It records the exact cross-compiler command and avoids silently replacing the
served diagnostic ZIP.

## Important constraints

- Stay on the `vibe` branch.
- The default package intentionally requests 32-bpp windowed mode; do not
  silently replace it with the old 8-bpp software path just to obtain pixels.
- Do not reset or clean the sibling BoxedWine or EDuke32 checkouts. Both are
  heavily dirty, and the verified GLX unbind/GL2 bridge work is currently
  local. A PE32 rebuild incorporates all current EDuke32 changes.
- Only this Wine repository may be edited. Do not edit, build, clean, reset,
  stash, or experiment with sibling `/Users/alonamir/dev/eduke32` or
  `/Users/alonamir/dev/boxedwine` unless the user explicitly authorizes that
  named action. Run only `/Users/alonamir/games/netduke32_v1.2.1/netduke32.exe`.
- `tools/patch-webgl-version.mjs` is required after an Emscripten runtime
  build: Wine's version parser rejects Emscripten's default `OpenGL ES ...`
  string.
- The historical `fputc` link failure (`__get_tp` /
  `emscripten_futex_wake`) came from compiling threaded objects but linking
  without `-pthread`. The current dirty BoxedWine targets use consistent
  pthread flags. The exact Control checkpoint command is in the
  [reproduction runbook](docs/reproduction.md).

## JIT cache

The JIT cache work is not a validation of the 32-bit GL path. It should only
be revisited after rendering works. If the runtime and packaged overlay are
compatible, regenerate the cache with:

```sh
make -C web-showcase cache
```

The cache is keyed to both the BoxedWine runtime and `netduke32.zip`; rebuild
it whenever either changes.
