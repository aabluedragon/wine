# Current browser checkpoint

Last verified: **2026-08-16 23:07 IDT (+03:00)**.

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
