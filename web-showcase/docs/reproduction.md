# Browser reproduction and rebuild runbook

Run commands from `/Users/alonamir/dev/wine` and stay on branch `vibe`.

## Verify both preserved checkpoints first

Verify the runtime, regression root, latest derivative root, and application:

```sh
shasum -a 256 \
  web-showcase/build-gl/boxedwine.js \
  web-showcase/build-gl/boxedwine.wasm \
  web-showcase/build-gl/tinycore-wine11-parent-inline-webgl-pci-glxshim.zip \
  web-showcase/build-gl/tinycore-wine11-parent-inline-webgl-pci-glxshim-gl2ext.zip \
  web-showcase/build-gl/netduke32.zip \
  web-showcase/tools/patch-wine11-gl2-extension-count.mjs
```

Expected values, in order:

```text
2acb3471e2b4c20064f4b2ffd90eac8c6719a0c3fd31da8f56383f791d91db1a
4a836d2267d708685ce6369bee5122bd6c36bc2e11c2483d921d201cd4222452
47e1d4d0961c62693960a756e8179ed2497a7784583c904de240c4d24e0c15fe
1bdc1c5b03f755ecd1aedef491f5fecfc2e2a18fe0f1d1e14f056e12810683cc
b27df9cc54377a0b5c1579512768cad149b0ef65a69b6752cde1a053999dda69
d1ce16731c0860f0ff1484a8840ba7bc5d3f6b09fb9d1d8db6d837e7423b508e
```

If any value differs, do not attribute the result to this checkpoint. Record
the new hashes and source diff before running it.

Start the no-cache, cross-origin-isolated server unless port 8082 already has
the correct process:

```sh
lsof -nP -iTCP:8082 -sTCP:LISTEN
node web-showcase/serve_gl.mjs web-showcase/build-gl
```

The second command stays in the foreground. Run it in a separate terminal.
Open the explicit latest URL from [current-state.md](current-state.md). Bare
`http://localhost:8082/` is not equivalent: it redirects to
`tinycore-wine11.zip`, which is a symlink to the unmodified base root.

For repeatable console and canvas inspection, reuse the existing CDP listener
if present. Otherwise start a dedicated headless Chrome profile:

```sh
lsof -nP -iTCP:9555 -sTCP:LISTEN
/Applications/Google\ Chrome.app/Contents/MacOS/Google\ Chrome \
  --headless=new \
  --remote-debugging-port=9555 \
  --no-first-run \
  --user-data-dir=/tmp/wine-web-perf-profile \
  about:blank
```

The Chrome command stays in the foreground. Enumerate page targets with:

```sh
curl -s http://127.0.0.1:9555/json/list | \
  jq -r '.[] | select(.type == "page") | [.id, .url, .title] | @tsv'
```

Enable/capture `Runtime.exceptionThrown` or keep the browser console open
before reloading. The guest textarea alone does not contain the current host
WASM exception.

### Regression control: canonical `glxshim`

Change only the URL's `root` value to
`tinycore-wine11-parent-inline-webgl-pci-glxshim.zip`. This preserved control
should have all of these landmarks:

- VibeBuild32 reaches `Setting video mode 640x480 (32-bpp windowed)`.
- BoxedWine logs GLX create/make-current calls for the game contexts.
- The old `wglMakeCurrent returned 0xc0000005` warning is absent.
- Wine rejects `glGetStringi` for its advertised OpenGL 2.0 context.
- The last query is `glGetIntegerv pname 33309` (`GL_NUM_EXTENSIONS`).
- The next exception is an execute access violation at `eip=00000000`.
- The browser canvas contains no real rendered frame.

The PE stack and disassembly prove that this execute violation is ImGui's NULL
`glGetStringi` slot. It is a regression control, not the latest boundary.

### Latest diagnostic: `glxshim-gl2ext`

Use the complete URL in [current-state.md](current-state.md), including
`root=tinycore-wine11-parent-inline-webgl-pci-glxshim-gl2ext.zip`. It should
advance past the regression control and show:

```text
trace:opengl:glGetIntegerv pname 33309, data ...
GLX: GL2 GL_NUM_EXTENSIONS -> 0
trace:opengl:glShadeModel mode 7425
```

Chrome currently follows that sequence with `RuntimeError: null function` in
`boxedwine.wasm`. The guest output stops without `c0000005`,
`wglMakeCurrent returned`, or `Caught signal`; the canvas backing remains 0x0.
That is the current reproducible failure.

Service, PlugPlay, RPC, Vulkan, and loader warnings also occur. They are not the
current render boundary because the game reaches its OpenGL loader afterward.

## Capture a useful result

Use this Wine debug value while localizing the current call:

```text
WINEDEBUG=+seh,+wgl,+opengl
```

For each material test, save or report:

- Wine and BoxedWine commit IDs and focused dirty diffs.
- SHA-256 of `boxedwine.js`, `boxedwine.wasm`, the root ZIP, and app ZIP.
- The full launch URL and guest command line printed by BoxedWine.
- The log from the last successful GLX call through the exception.
- Browser console errors.
- A screenshot and the canvas backing dimensions, for example:

```js
[...document.querySelectorAll('canvas')].map((canvas) => ({
  width: canvas.width,
  height: canvas.height,
  clientWidth: canvas.clientWidth,
  clientHeight: canvas.clientHeight,
}))
```

Do not use a boot message or a nonzero CSS canvas size as a pass condition.

## Rebuild the current Control runtime

The checkpoint runtime is a pthread-enabled, interpreter-only diagnostic build.
It was built from the dirty sibling BoxedWine checkout with Emscripten 4.0.23:

```sh
. /Users/alonamir/dev/emsdk/emsdk_env.sh
MAKEFLAGS=-j9 make -C /Users/alonamir/dev/boxedwine/project/emscripten \
  BUILD_DIR=Build/Control \
  EXTRA_CPP_FLAGS='-pthread -DBOXEDWINE_WEB_INDEXED_RENDERER' \
  SHELL_FILE=shell.html \
  EXTRA_LD_FLAGS='-pthread -sPTHREAD_POOL_SIZE=8' \
  ASSET_FILES='boxedwine.css boxedwine-shell.js'
node web-showcase/tools/patch-webgl-version.mjs \
  /Users/alonamir/dev/boxedwine/project/emscripten/Build/Control/boxedwine.js
cp /Users/alonamir/dev/boxedwine/project/emscripten/Build/Control/boxedwine.html \
   /Users/alonamir/dev/boxedwine/project/emscripten/Build/Control/boxedwine.css \
   /Users/alonamir/dev/boxedwine/project/emscripten/Build/Control/boxedwine-shell.js \
   /Users/alonamir/dev/boxedwine/project/emscripten/Build/Control/boxedwine.js \
   /Users/alonamir/dev/boxedwine/project/emscripten/Build/Control/boxedwine.wasm \
   web-showcase/build-gl/
```

After every rebuild:

```sh
cmp /Users/alonamir/dev/boxedwine/project/emscripten/Build/Control/boxedwine.js \
    web-showcase/build-gl/boxedwine.js
cmp /Users/alonamir/dev/boxedwine/project/emscripten/Build/Control/boxedwine.wasm \
    web-showcase/build-gl/boxedwine.wasm
shasum -a 256 web-showcase/build-gl/boxedwine.js \
                 web-showcase/build-gl/boxedwine.wasm
```

Update `current-state.md` with the new hashes before interpreting a browser
reload. `web-showcase/serve_gl.mjs` sends `Cache-Control: no-store`, but a new
URL query or a fresh tab is still useful when comparing builds.

The source marker for the extension-count handler can be checked with:

```sh
strings web-showcase/build-gl/boxedwine.wasm | \
  rg 'GLX: GL2 GL_NUM_EXTENSIONS -> 0'
```

Its presence only proves that source text was linked. The canonical root does
not take the handler; the `gl2ext` derivative does, and its live marker proves
the complete query path.

## Rebuild the Wine 11.0 `gl2ext` derivative

The latest root is a non-destructive derivative of the canonical `glxshim`
root. It changes only
`opt/wine/lib/wine/i386-unix/opengl32.so`. The patch script validates the
entire input/output hashes and the surrounding code before changing two branch
bytes:

```sh
VIBE_GL2_STAGE=$(mktemp -d /tmp/vibe-gl2-root.XXXXXX)
VIBE_GL2_CANONICAL=/Users/alonamir/dev/wine/web-showcase/build-gl/tinycore-wine11-parent-inline-webgl-pci-glxshim.zip
VIBE_GL2_OUTPUT=/Users/alonamir/dev/wine/web-showcase/build-gl/tinycore-wine11-parent-inline-webgl-pci-glxshim-gl2ext-rebuilt.zip

unzip -q "$VIBE_GL2_CANONICAL" \
  opt/wine/lib/wine/i386-unix/opengl32.so \
  -d "$VIBE_GL2_STAGE"
shasum -a 256 \
  "$VIBE_GL2_STAGE/opt/wine/lib/wine/i386-unix/opengl32.so"
node web-showcase/tools/patch-wine11-gl2-extension-count.mjs \
  "$VIBE_GL2_STAGE/opt/wine/lib/wine/i386-unix/opengl32.so"
shasum -a 256 \
  "$VIBE_GL2_STAGE/opt/wine/lib/wine/i386-unix/opengl32.so"

cp "$VIBE_GL2_CANONICAL" "$VIBE_GL2_OUTPUT"
(cd "$VIBE_GL2_STAGE" && zip -q -u "$VIBE_GL2_OUTPUT" \
  opt/wine/lib/wine/i386-unix/opengl32.so)
unzip -p "$VIBE_GL2_OUTPUT" \
  opt/wine/lib/wine/i386-unix/opengl32.so | shasum -a 256
```

The module hashes before and after must be:

```text
60bb64157afc62067ee2298fac0a96943deaf63783efd30c91b8ad8c305eaea0
a3dceaa1f93019a76da43c9b5019c9847b25abc385af2e20c6da47821649287b
```

At file offsets `0x6aac0` and `0x6aac1`, bytes `74 65` become `90 90`.
This disables Wine 11.0's special `GL_NUM_EXTENSIONS` return so the GL 2 query
falls through to the guest driver. The preserved derivative has outer SHA-256
`1bdc1c5b03f755ecd1aedef491f5fecfc2e2a18fe0f1d1e14f056e12810683cc`.
A rebuilt ZIP can have a different outer hash because of member timestamps;
compare its inner module hash and payload list. Never overwrite the canonical
root.

## Custom root and guest GL shim provenance

There is not yet a tracked end-to-end recipe from the downloaded TinyCore root
to `tinycore-wine11-parent-inline-webgl-pci.zip` (SHA-256
`f00cd62c26e6567e8769c74a38023780632f1d31636843b551e878de5b381976`).
That predecessor was assembled during the investigation from manual Wine boot,
dependency, WebGL, Mesa/LLVM, PCI, and driver overlays. Preserve the canonical
root ZIP; a clean clone cannot currently recreate all of it.

Known unreproduced predecessor operations include manual changes to the Wine
prefix registry and `wine.inf`, a 28-byte i386 boot patch in Wine 11.0
`ntdll.so` at file offset `0x11e19`, and the one-byte `use_egl` patch in
`i386-unix/winex11.so` at file offset `0x6bb30`. Mesa/X11/DRM/swrast/LLVM 15,
libelf, and pciaccess payloads came from retained TinyCore x86 `.tcz` packages,
but no script records their download, extraction, copy order, or symlink
handling. The exact inputs can be forensically recovered from this workspace;
the original end-to-end command sequence cannot.

The final GLX-shim step is recoverable at the content level. The observed
archive chain is:

| Archive | SHA-256 | Relationship |
| --- | --- | --- |
| `tinycore-wine11-parent-inline-webgl-pci.zip` | `f00cd62c26e6567e8769c74a38023780632f1d31636843b551e878de5b381976` | Last dependency-overlay root. |
| `tinycore-wine11-parent-inline-webgl-pci-glxfix2.zip` | `f4f2ee54de25be96fbbe71b88373a41c49d33361f79946b64e6a3f9d1d80b9f5` | Same file payloads; reinsertion changed only `winex11.drv.so` mode/time metadata. |
| `tinycore-wine11-parent-inline-webgl-pci-glxfix.zip` | `47e1d4d0961c62693960a756e8179ed2497a7784583c904de240c4d24e0c15fe` | Replaces `lib/libGL.so.1.2.0` with the custom f090 shim. |
| `tinycore-wine11-parent-inline-webgl-pci-glxshim.zip` | `47e1d4d0961c62693960a756e8179ed2497a7784583c904de240c4d24e0c15fe` | Byte-for-byte copy of `glxfix`; canonical name. |

The unchanged `winex11.drv.so` wrapper payload has SHA-256
`0148734b198210e5fbb95a3ba542a0bc269dec524b41a35e836b4c7c179d1516`.
The command that changed its ZIP metadata is not recoverable and is irrelevant
to execution.

The exact custom guest `libGL` can be reconstructed from another preserved
experimental root:

```sh
VIBE_GL_TMP_DIR=$(mktemp -d /tmp/vibe-gl-rebuild.XXXXXX)
unzip -q -j \
  web-showcase/build-gl/tinycore-wine11-webgl-localresolver.zip \
  lib/libGL.so.1 -d "$VIBE_GL_TMP_DIR"
shasum -a 256 "$VIBE_GL_TMP_DIR/libGL.so.1"
node web-showcase/tools/patch-glx-make-current.mjs \
  "$VIBE_GL_TMP_DIR/libGL.so.1"
shasum -a 256 "$VIBE_GL_TMP_DIR/libGL.so.1"
```

The before/after hashes must be:

```text
2b1a5f179d1fb68604eb68421a271331690f5ff8cd31cff809e3637035fa79af
f09030c9fc8cbfa71e04985629217159aa61332363d014e12d39d993ea850c2f
```

The source archive for that input,
`tinycore-wine11-webgl-localresolver.zip`, has SHA-256
`47546c2801e2959b571b191ad12d8f787f46e17d9f9d48b56275869268152d54`.
The final root contains two regular files—`lib/libGL.so.1` and
`lib/libGL.so.1.2.0`—with the f090 hash; neither is a symlink.

To produce a content-equivalent derivative without overwriting either input:

```sh
VIBE_ROOT_STAGE=$(mktemp -d /tmp/vibe-root-stage.XXXXXX)
mkdir -p "$VIBE_ROOT_STAGE/lib"
cp "$VIBE_GL_TMP_DIR/libGL.so.1" "$VIBE_ROOT_STAGE/lib/libGL.so.1"
cp "$VIBE_GL_TMP_DIR/libGL.so.1" "$VIBE_ROOT_STAGE/lib/libGL.so.1.2.0"
cp web-showcase/build-gl/tinycore-wine11-parent-inline-webgl-pci.zip \
  web-showcase/build-gl/tinycore-wine11-glxshim-rebuilt.zip
(cd "$VIBE_ROOT_STAGE" && zip -q -u \
  /Users/alonamir/dev/wine/web-showcase/build-gl/tinycore-wine11-glxshim-rebuilt.zip \
  lib/libGL.so.1 lib/libGL.so.1.2.0)
unzip -p web-showcase/build-gl/tinycore-wine11-glxshim-rebuilt.zip \
  lib/libGL.so.1.2.0 | shasum -a 256
```

ZIP metadata/order means the outer archive hash may differ; verify the inner
files and behavior. Do not replace the canonical artifact in place.

For a fresh source build, use sibling
`tools/opengl/buildgl.sh` on Debian 12 amd64 with 32-bit GCC and GL/GLU headers.
It compiles `gl.c` with `-m32 -march=i586`. The current
`tools/opengl/glxfunctions.h` contains the intended inline resolver and
make-current adaptation. The sibling `tools/opengl/libGL.so.1` file itself is
stale (SHA-256
`92cfe73f20d6c2772c36b6f2376c83490e27f4337f8755235346b130f51c52fb`)
and must not be copied as though it reflected that source. A new source build
will not necessarily reproduce the historical outer ZIP byte-for-byte; record
its compiler, build ID, and SHA-256.

```sh
cd /Users/alonamir/dev/boxedwine/tools/opengl
./buildgl.sh
file libGL.so.1
shasum -a 256 libGL.so.1
```

The exact source snapshot that generated the 2b1 input is not preserved. Its
ELF comment identifies GCC `Debian 12.2.0-14+deb12u1`; its build ID is
`b124d9adf8241f669ab7725fb56bc52a1e885903`. The current source has an
additional source-level `draw == read` adaptation, so a fresh build is useful
for forward progress but is not expected to be byte-identical to 2b1/f090.

## Tracked JIT recipe is a different build

The showcase Makefile's normal path is:

```sh
make -C web-showcase runtime app
make -C web-showcase serve
```

It builds sibling BoxedWine `Build/Jit`, downloads the unmodified root into
`web-showcase/build/`, and serves port 8080. It does **not** recreate the
Control/glxshim checkpoint above.

The earlier `fputc` link failure with unresolved `__get_tp` and
`emscripten_futex_wake` was caused by mixing threaded objects with a
non-threaded link. It reproduced with Emscripten 4.0.23 and 6.0.5 when the
flags were inconsistent. The current dirty BoxedWine `jit` target now passes
`-pthread` during both compilation and linking and sets a pthread pool; treat
that issue as a cleared configuration problem, not the current OpenGL blocker.

## Optional application-side ImGui GL2 hardening

This source fix has been audited but is **not applied, compiled, or
browser-tested** at the checkpoint. The bridge/root fix already clears the
specific ImGui crash, but the application should not use GL 3 indexed
extension APIs on an advertised GL 2 context.

In
`/Users/alonamir/dev/eduke32/source/imgui/src/imgui_impl_opengl3.cpp`, constrain
the `GL_NUM_EXTENSIONS`/`glGetStringi` loop to:

```cpp
bd->GlVersion >= 300 && glGetStringi != nullptr
```

For `bd->GlVersion < 300`, use an exact-token scan of
`glGetString(GL_EXTENSIONS)` when checking `GL_ARB_clip_control`. For an
advertised GL 3+ context with a NULL `glGetStringi` slot, skip extension
detection rather than making the legacy query, which is invalid in core
profiles. Engine GLAD already makes the correct legacy/indexed version split;
do not patch it.

The vendored backend is ImGui 1.92.6 WIP/19259, imported from upstream commit
`76860017`. Upstream master `46d39d56` and docking `83f66862` still contain the
same unconditional loop as of 2026-08-16, so an ImGui update alone does not
remove the bug.

Before a PE32 build, preserve and record the dirty EDuke32 tree:

```sh
git -C /Users/alonamir/dev/eduke32 branch --show-current
git -C /Users/alonamir/dev/eduke32 rev-parse HEAD
git -C /Users/alonamir/dev/eduke32 status --short
git -C /Users/alonamir/dev/eduke32 diff -- \
  source/imgui/src/imgui_impl_opengl3.cpp GNUmakefile
i686-w64-mingw32-g++ --version | head -1
```

At handoff, the branch is `vibe` at `673f07bd...`, the tree contains extensive
modified/deleted/untracked Vibe work, `obj-win32/` is untracked, and the cross
compiler reports GCC 16.1.0. Do not clean, reset, stash, or touch timestamps.
A normal build incorporates all current dirty source, so it is not an isolated
ImGui-only binary.

Build and identify the PE32 executable with:

```sh
make -C /Users/alonamir/dev/eduke32 -j8 \
  PLATFORM=WINDOWS CROSS=i686-w64-mingw32- \
  obj=obj-win32 HAVE_FLAC=0 vibebuild32
file /Users/alonamir/dev/eduke32/vibebuild32.exe
shasum -a 256 /Users/alonamir/dev/eduke32/vibebuild32.exe
```

## Repackage the application overlay

The recipe depends on local/licensed EDuke32 files and copies
`vibebuild32.exe` as `netduke32.exe`. For an A/B-safe diagnostic, run only the
app target from a temporary Makefile directory. This keeps both the served
checkpoint ZIP and the repository's existing untracked `web-showcase/staging`
tree intact:

```sh
VIBE_APP_PACKAGE_DIR=$(mktemp -d /tmp/vibe-web-app.XXXXXX)
cp web-showcase/Makefile "$VIBE_APP_PACKAGE_DIR/Makefile"
make -C "$VIBE_APP_PACKAGE_DIR" OUT=build app
cp "$VIBE_APP_PACKAGE_DIR/build/netduke32.zip" \
  web-showcase/build-gl/netduke32-imgui-guard.zip
shasum -a 256 web-showcase/build-gl/netduke32-imgui-guard.zip
unzip -p web-showcase/build-gl/netduke32-imgui-guard.zip \
  netduke32/netduke32.exe | shasum -a 256
unzip -p web-showcase/build-gl/netduke32-imgui-guard.zip \
  netduke32/netduke32.cfg | \
  rg 'ForceSetup|Screen(BPP|Width|Height|Mode)'
```

Change only the explicit launch URL's `app` parameter to
`netduke32-imgui-guard.zip`. Expected configuration is `ForceSetup = 0`, BPP
32, width 640, height 480, and mode 0.

Only when intentionally replacing the diagnostic checkpoint use:

```sh
make -C web-showcase OUT=build-gl app
```

That target deletes/recreates `web-showcase/staging` and overwrites
`build-gl/netduke32.zip`. Inspect the staging tree first and do not run it if it
contains work that must be preserved. Immediately record the new outer ZIP and
inner EXE hashes and update the exact URL in `current-state.md`; never keep the
old artifact identity after replacement.

## Passing criteria

A passing test requires all of the following:

- The process remains alive through GL initialization with neither the old
  make-current `c0000005`, the ImGui EIP-0 call, nor a host WASM NULL call.
- Logs/configuration confirm 640x480, 32-bpp OpenGL with no 8-bpp fallback.
- The canvas has nonzero backing dimensions and meaningful non-black
  game/menu/desktop pixels.
- Two temporally separated screenshots or pixel checks prove frames update.
- Focused keyboard and mouse actions produce observable application changes.
- The result records source commits/diffs, runtime JS/WASM, root, and app
  hashes, the exact URL, decisive logs, screenshot evidence, and input evidence.

Only after these pass should JIT-cache and gameplay performance work resume.
