# WASM 32-bit OpenGL investigation

This is the evidence and decision record. For the mutable, exact checkpoint,
read [current-state.md](current-state.md).

## Current boundary

As of 2026-08-16 23:07 IDT, VibeBuild32 reaches 640x480 32-bpp video setup and
successfully crosses the Wine/GLX make-current boundary that used to crash. A
Wine 11.0 `gl2ext` derivative also clears the subsequent ImGui NULL
`glGetStringi` call. The current boundary is now a browser-side WASM NULL call
immediately after `glShadeModel(GL_SMOOTH)`.

The decisive sequence is:

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

Chrome DevTools emitted the console event and `Runtime.exceptionThrown`. Guest
output then stayed unchanged with no `c0000005`, signal, or Wine abort. The
canvas backing remained 0x0. This is later than both cleared guest failures.

The next source change should be confined to BoxedWine
`source/opengl/sdl/sdlgl.cpp`, beside the existing Emscripten WebGL helpers:

```cpp
static void OPENGL_CALL_TYPE wasmGlShadeModel(GLenum mode)
{
    if (mode != GL_SMOOTH)
        kwarn_fmt("WebGL cannot emulate glShadeModel(%u); keeping smooth interpolation",
                  (unsigned)mode);
}
```

After the generated `SDL_GL_GetProcAddress` assignments, use
`if (!pglShadeModel) pglShadeModel = wasmGlShadeModel;` under Emscripten. This
source change is not yet applied or rebuilt.

Recheck the exact indirect-call PC with the installed Homebrew LLVM:

```sh
/opt/homebrew/opt/llvm/bin/llvm-objdump -d --no-show-raw-insn \
  --start-address=0x2a5780 --stop-address=0x2a57b1 \
  web-showcase/build-gl/boxedwine.wasm
```

The addresses apply only to WASM SHA-256 `4a836d22...`; re-localize them after
any rebuild.

## Evidence versus inference

| Statement | Status | Evidence |
| --- | --- | --- |
| The application negotiates the bridge as desktop OpenGL 2.0. | Observed | Wine `+opengl` version and resolver traces. |
| Wine rejects `glGetStringi` because it requires GL 3.0. | Observed | `wrap_wglGetProcAddress` warning immediately before initialization calls. |
| The app queries `GL_NUM_EXTENSIONS`, then executes NULL. | Observed | `pname 33309` followed by execute AV at EIP 0. |
| ImGui's extension loop calls `glGetStringi` once for each reported extension. | Observed in source | `eduke32/source/imgui/src/imgui_impl_opengl3.cpp` lines around 417-420. |
| The NULL call is ImGui's indexed extension query. | Proven | Guest stack return `0x005a0261`; PE call at `0x005a025b` dereferences NULL global `0x0165ee88` with `GL_EXTENSIONS` on the stack. |
| Wine 11.0 intercepts `GL_NUM_EXTENSIONS` before the guest driver. | Proven | `dlls/opengl32/unix_wgl.c:get_integer()` returns `ctx->extension_count`; canonical root returns 7 without the BoxedWine marker. |
| Falling through to BoxedWine returns zero and clears the ImGui crash. | Proven | Hash-pinned `opengl32.so` branch patch produces the marker, zero count, and execution advances to `glShadeModel`. |
| The next host exception follows `glShadeModel(GL_SMOOTH)`. | Observed | Last guest line is mode 7425; CDP then reports `RuntimeError: null function` in WASM. |
| The NULL host target is specifically `pglShadeModel`. | Proven | Guest opcode 216 and generated handler shape match WASM disassembly; fault PC `0x2a57ad` is `return_call_indirect 0` after loading the slot, and WebGL's resolver has no ShadeModel entry. |

Do not advertise fake GL 3 support merely to make the resolver return
`glGetStringi`. The current bridge uses a WebGL 1/GLES 2 compatibility path;
claiming GL 3 requires a deliberate WebGL 2 implementation and capability
audit.

## Cleared make-current root cause

The previous visible failure was:

```text
warn:opengl:wglMakeCurrent wglMakeCurrent returned 0xc0000005
```

Live SEH tracing mapped its fault to packaged Wine 11.0 `win32u.so`, base
`0x12660000`, offset `0x0cceb5`, in `context_sync_drawables()` at Wine 11.0
`dlls/win32u/opengl.c` around line 1687. `old_read` was freed-memory poison
`0x00fefefe`; `opengl_drawable_set_context(old_read, NULL)` tried to read its
function table at offset `0x0c`.

The stale context originated in BoxedWine
`source/opengl/sdl/sdlgl.cpp`. A local guard rejected every missing context
before the existing unbind branch:

```cpp
if (!context || !context->context) return false;
```

For valid GLX unbind, `contextId == 0` and no context object is expected. The
false return prevented SDL/WebGL unbind. Wine then deleted its short-lived
probe context while `NtCurrentTeb()->glContext` still referred to it. The next
game make-current exchanged the freed draw/read objects and crashed.

The verified correction is:

```cpp
if (contextId && (!context || !context->context)) return false;
```

After rebuilding, trace output showed probe drawable releases, game context
creation, entry into Wine `x11drv_make_current`, and BoxedWine
`GLX: glXMakeCurrent`. The old `wglMakeCurrent returned 0xc0000005` no longer
appears. Preserve this fix.

## Cleared extension-count dispatch root cause

The dirty BoxedWine `source/opengl/glcommon.cpp` currently contains an
Emscripten-only branch in `glcommon_glGetIntegerv()`:

```cpp
if (pname == GL_NUM_EXTENSIONS) {
#ifdef __EMSCRIPTEN__
    klog("GLX: GL2 GL_NUM_EXTENSIONS -> 0");
    cpu->memory->writed(ARG2, 0);
#else
    cpu->memory->writed(ARG2, cpu->thread->process->numberOfExtensions);
#endif
}
```

The exact marker is linked into the current WASM. It was absent with the
canonical root because matched Wine 11.0
`dlls/opengl32/unix_wgl.c:get_integer()` special-cases
`GL_NUM_EXTENSIONS` and returns `ctx->extension_count` before the Unix driver
call. That value was 7, even though Wine correctly refused to expose the GL 3
`glGetStringi` entry point.

The stripped canonical
`/opt/wine/lib/wine/i386-unix/opengl32.so` has SHA-256
`60bb64157afc62067ee2298fac0a96943deaf63783efd30c91b8ad8c305eaea0`.
At file offset `0x6aac0`, its `74 65` branch selects the synthetic count. The
hash/context-pinned
`web-showcase/tools/patch-wine11-gl2-extension-count.mjs` changes those bytes
to `90 90`, producing module SHA-256
`a3dceaa1f93019a76da43c9b5019c9847b25abc385af2e20c6da47821649287b`.
The following comparison then sends enum `0x821d` through the normal driver
path.

The resulting root is
`tinycore-wine11-parent-inline-webgl-pci-glxshim-gl2ext.zip`, SHA-256
`1bdc1c5b03f755ecd1aedef491f5fecfc2e2a18fe0f1d1e14f056e12810683cc`.
Its live marker and advance to `glShadeModel` prove the diagnosis. Keep the
canonical root unchanged as a regression control. See
[reproduction.md](reproduction.md) for reconstruction commands.

## Exact NULL-call mapping

At the execute exception, guest `ESP` is `0x7f30f868`. Reading the guest stack
through the BoxedWine memory backing gives:

```text
[ESP]     0x005a0261
[ESP+4]   0x00001f03
```

Disassembly of the packaged PE (SHA-256
`149dc75560db8dd358bb50911fb5d480ef113335164d7e2c7045a0cc00cc5134`)
shows:

```text
005a022a: movl  $0x821d,(%esp)       ; GL_NUM_EXTENSIONS
005a0231: calll *0x0165ee70          ; glGetIntegerv
005a0237: movl  -0x98(%ebp),%edx
005a0240: testl %edx,%edx
005a0242: jle   0x005a028b
005a0254: movl  $0x1f03,(%esp)       ; GL_EXTENSIONS
005a025b: calll *0x0165ee88          ; NULL glGetStringi slot
005a0261: subl  $0x8,%esp            ; saved return address
```

This matches the ImGui source loop and closes the uncertainty about the EIP-0
caller. The Wine 11.0 early-return path above now also explains the inconsistent
positive count. This section is retained as cleared-failure evidence.

Recheck the packaged instructions without modifying the overlay:

```sh
VIBE_APP_TMP_DIR=$(mktemp -d /tmp/vibe-app.XXXXXX)
unzip -q -j web-showcase/build-gl/netduke32.zip \
  netduke32/netduke32.exe -d "$VIBE_APP_TMP_DIR"
shasum -a 256 "$VIBE_APP_TMP_DIR/netduke32.exe"
objdump -d --start-address=0x5a0220 --stop-address=0x5a0280 \
  "$VIBE_APP_TMP_DIR/netduke32.exe"
```

The PE is stripped to an external PDB, so use the fixed addresses and exact
artifact hash when comparing this disassembly.

## Audited ImGui GL2 hardening

The application-side source remains unmodified at handoff, but its correct
behavior is known. In
`eduke32/source/imgui/src/imgui_impl_opengl3.cpp`, use indexed extension
enumeration only when `bd->GlVersion >= 300 && glGetStringi != nullptr`. Below
GL 3, inspect the legacy `glGetString(GL_EXTENSIONS)` value with exact token
boundaries for `GL_ARB_clip_control`. For advertised GL 3+ with a missing
indexed function, skip extension probing rather than make a legacy query that
may be invalid in a core profile.

The vendored ImGui backend is 1.92.6 WIP/19259 from upstream commit
`76860017`. Upstream master `46d39d56` and docking `83f66862` still contain the
same unconditional loop as of 2026-08-16, so upgrading alone is not a fix.
Engine GLAD already makes the correct version split and should be left alone.

This hardening has not been applied, built, or browser-tested. The dirty-tree
PE32 command and A/B-safe repack procedure are in
[reproduction.md](reproduction.md#optional-application-side-imgui-gl2-hardening).

## Relevant source map

Wine checkout or Wine 11.0 tag:

- `dlls/win32u/opengl.c` — context state and GL dispatch.
- Wine 11.0 `dlls/opengl32/unix_wgl.c` — `get_integer()` synthetic extension
  count and Unix-side GL wrappers.
- `dlls/winex11.drv/opengl.c` — X11 driver make-current and GL function table.
- `dlls/winex11.drv/x11drv_main.c` — local source defaults `use_egl = FALSE`,
  but this newer source did not build the packaged driver.
- `include/wine/opengl_driver.h` — local ABI version 38; Wine 11.0 uses 37.

Sibling BoxedWine:

- `source/opengl/sdl/sdlgl.cpp` — logical GLX contexts, browser WebGL context,
  make-current/unbind, and SDL function loading.
- `source/opengl/glcommon.cpp` — int-0x99 GL handlers, extension filtering, and
  `callOpenGL()`.
- `source/opengl/glfunctions.h` — generated standard handlers, including
  `ShadeModel`.
- `platform/sdl/knativescreenSDL.cpp` — browser/window presentation.
- `source/emulation/cpu/normal/normal_other.h` — interpreter int-0x99 entry.
- `source/emulation/cpu/common/common_other.cpp` — common int-0x99 entry.
- `tools/opengl/gl.c` — guest GL shim; `glGetIntegerv` uses opcode 32.
- `tools/opengl/gldef.h` — GL opcode numbers.
- `tools/opengl/glxfunctions.h` — guest GLX declarations/opcodes.

Sibling EDuke32:

- `source/build/src/winlayer.cpp` — legacy desktop-GL initialization; current
  `glShadeModel(GL_SMOOTH)` call is around line 2609.
- `source/imgui/src/imgui_impl_opengl3.cpp` — loader initialization and the
  extension enumeration loop around lines 417-420.

## Artifact and ABI facts

- The current runtime is pthread-enabled `Build/Control`, interpreter-only. A
  JIT cache cannot affect this failure.
- Both comparison roots are Wine 11.0. The local Wine checkout is based on
  11.14.
- `WINE_OPENGL_DRIVER_VERSION` changed from 37 to 38 between them and the
  OpenGL function-table layouts changed. Never overlay only a current-tree
  `win32u.so` or `winex11.so` into this root.
- The actual native driver is
  `/opt/wine/lib/wine/i386-unix/winex11.so`, not the 84 KiB
  `winex11.drv.so` wrapper or the 2.5 KiB PE stub.
- The custom guest `/lib/libGL.so.1.2.0` already reaches BoxedWine for
  `glXChooseVisual`, `glXCreateContext`, and `glXMakeCurrent`. The earlier claim
  that the failure occurred before the useful GLX handler is obsolete.
- The similarly named `glxshim` and `glxfix` roots are byte-identical. The
  `glxfix2` root is different; do not substitute it by name.
- The packaged guest `libGL` has SHA-256
  `f09030c9fc8cbfa71e04985629217159aa61332363d014e12d39d993ea850c2f`;
  sibling `tools/opengl/libGL.so.1` has SHA-256
  `92cfe73f20d6c2772c36b6f2376c83490e27f4337f8755235346b130f51c52fb`
  and does not match the packaged resolver. The packaged core
  `glGetIntegerv` still uses int-0x99 opcode `0x20`; the `gl2ext` live marker
  proves this dispatch when Wine permits fallthrough.

See [wine11-driver-debugging.md](wine11-driver-debugging.md) for full driver
provenance and a matched rebuild procedure.

## Experiment ledger

| Stage | Change/control | Result |
| --- | --- | --- |
| Thread-model link | Added `-pthread` consistently to compile/link and a pthread pool. | Cleared historical `fputc` / `__get_tp` / futex link errors. |
| Boot and desktop | Browser root/service experiments and explicit Wine desktop/direct launch variants. | Application reaches game initialization and 32-bpp setup. |
| GL version | Desktop-compatible `2.0 BoxedWine WebGL` report. | Wine accepts the WebGL-backed GL version. |
| GLX resolver/shim | Custom unstripped guest `libGL.so.1.2.0`. | GLX choose/create/make-current traps reach BoxedWine. |
| Interpreter control | Reproduced without x86-to-WASM JIT. | Proved the renderer failure is not JIT warm-up. |
| GLX unbind | Allow `contextId == 0` to reach the clear-current path. | Cleared stale Wine context and old non-NULL C0000005. |
| Extension-count handler | Return zero in `glcommon_glGetIntegerv` under Emscripten. | Compiled correctly, but canonical Wine intercepts the query before it reaches the handler. |
| Wine 11.0 GL2 count | Disable the matched `get_integer()` synthetic-count branch in a derivative root. | Handler logs, returns zero, clears ImGui EIP-0, and advances to `glShadeModel`. |
| Shade model | No fix yet; observe the first call after the cleared ImGui loop. | Browser throws `RuntimeError: null function`; canvas remains 0x0. |

## Next experiments

1. Add an Emscripten-only `wasmGlShadeModel()` fallback in BoxedWine
   `source/opengl/sdl/sdlgl.cpp`, assigned only when the generated
   `pglShadeModel` load returns NULL. No-op `GL_SMOOTH`; warn and retain smooth
   interpolation for other values. Preserve native desktop dispatch.
2. Rebuild Control and test the exact `gl2ext` root. Do not enable
   `LEGACY_GL_EMULATION` or the broad incomplete BoxedWine ES layer.
3. Capture the next guest line and CDP exception before fixing another call.
   Expect more desktop fixed-function entry points to need deliberate WebGL
   compatibility; do not blanket-call NULL pointers or claim fake GL support.
4. Independently harden the ImGui backend: indexed enumeration only for GL 3+
   with non-NULL `glGetStringi`, legacy exact-token string scan below GL 3.
   Use an A/B-named app ZIP because EDuke32 is heavily dirty.
5. Continue until a meaningful frame updates and input works; then repeat under
   the JIT runtime and resume performance work.

## Do not regress

- Do not reintroduce rejection of `contextId == 0` in BoxedWine GL unbind.
- Do not switch to 8-bpp software output to claim pixels.
- Do not copy Wine 11.14 modules into the Wine 11.0 root.
- Do not infer runtime identity from a ZIP filename or browser tab.
- Do not call context creation, video-mode selection, or a black canvas a pass.
