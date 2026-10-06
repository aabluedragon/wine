# Building Wine for macOS (Apple Silicon)

This fork is optimized for building and running Wine on macOS. It is based on
`wine-11.14` and carries a fix for a legacy-OpenGL extension bug that breaks
GL 1.x/2.x applications on macOS (see the commit history), plus this recipe.

The result is a self-contained, movable `wine-macos/` install that runs 32-bit
and 64-bit Windows apps on Apple Silicon via Rosetta 2, with Vulkan (MoltenVK),
TrueType fonts (freetype) and TLS (gnutls) working out of the box.

## Prerequisites

- Xcode Command Line Tools
- Rosetta 2 (`softwareupdate --install-rosetta`)
- Homebrew (arm64) packages: `brew install bison mingw-w64`
  (system bison 2.3 is too old; mingw-w64 provides the PE cross-compilers)

## 1. Build x86_64 Unix-side dependencies

Wine's Unix side must be x86_64, so its libraries must be too — arm64 Homebrew
libraries won't link. `macos-build-deps.sh` builds freetype, gmp, nettle and
gnutls from source into `deps/` as x86_64 dylibs:

```sh
./macos-build-deps.sh   # edit ROOT at the top first
```

## 2. Configure and build Wine

```sh
mkdir build && cd build
PATH="/opt/homebrew/opt/bison/bin:/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin" \
PKG_CONFIG_PATH=<deps>/lib/pkgconfig \
CPPFLAGS="-I<deps>/include" LDFLAGS="-L<deps>/lib" \
MACOSX_DEPLOYMENT_TARGET=12.0 \
arch -x86_64 ../configure --prefix=<install-dir> \
  --enable-archs=i386,x86_64 --disable-tests --without-x
arch -x86_64 make -j$(sysctl -n hw.ncpu)
arch -x86_64 make install
```

`--enable-archs=i386,x86_64` enables the new WoW64 mode so 32-bit Windows
programs work without 32-bit Unix libraries (macOS has none).

## 3. Bundle the dependency dylibs (make the install movable)

Wine dlopens freetype/gnutls by SONAME, which dyld can't find in a custom
prefix. Bundle them next to Wine's Unix libraries and reference them via
`@rpath` (every Wine .so carries an `@loader_path/` rpath):

```sh
U=<install-dir>/lib/wine/x86_64-unix
cp <deps>/lib/{libfreetype.6,libgnutls.30,libnettle.8,libhogweed.6,libgmp.10}.dylib "$U/"
cd "$U"
for l in *.dylib; do
  install_name_tool -id "@rpath/$l" "$l"
  for dep in libnettle.8.dylib libhogweed.6.dylib libgmp.10.dylib; do
    install_name_tool -change <deps>/lib/$dep "@loader_path/$dep" "$l" 2>/dev/null
  done
  codesign -f -s - "$l"
done
```

Then point the build's `include/config.h` at the bundled names **before**
`make` (or re-run make after editing and reinstall):

```c
#define SONAME_LIBFREETYPE "@rpath/libfreetype.6.dylib"
#define SONAME_LIBGNUTLS "@rpath/libgnutls.30.dylib"
```

## 4. Vulkan via MoltenVK

Download `MoltenVK-macos.tar` from the Khronos MoltenVK releases, then:

```sh
lipo MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib -thin x86_64 -output "$U/libMoltenVK.dylib"
codesign -f -s - "$U/libMoltenVK.dylib"
```

and add to `include/config.h` (rebuild + reinstall afterwards):

```c
#define SONAME_LIBVULKAN "@rpath/libMoltenVK.dylib"
```

MoltenVK's install name is already `@rpath/libMoltenVK.dylib`, so no further
patching is needed.

## Notes and gotchas

- **OpenGL**: winemac.drv offers at most GL 2.1 in compatibility mode (4.1
  core-only). Apps requiring a GL 3+ compatibility context cannot work; apps
  fine with 2.1 work correctly with this fork's extension fix. Two further
  gaps in that 2.1 profile are papered over by this fork, because Windows
  drivers expose both on compatibility contexts and applications assume they
  are always present:
  - **Sampler objects** are emulated on top of texture parameters, so
    `GL_ARB_sampler_objects` is advertised and usable (Build engine ports such
    as NetDuke32 load the entry points only when the extension is present, but
    call them unconditionally, and crash on the resulting NULL pointers).
  - **Buffer updates** avoid Apple's lack of buffer renaming: updating a
    buffer the GPU may still be reading synchronizes with it, costing ~250us
    per call, which reduces engines that refill a vertex buffer before every
    draw to ~23fps. Full-buffer updates respecify the storage (orphaning) and
    partial ones go through an unsynchronized mapping with an explicit range
    flush (`GL_APPLE_flush_buffer_range`).
  - **Unknown context attributes** no longer fail context creation;
    `WGL_CONTEXT_OPENGL_NO_ERROR_ARB` and the robustness reset-notification
    strategy are accepted and ignored. SDL2 requests the former, and rejecting
    it made SDL fall back to 8-bpp software rendering.
- **Audio**: Wine's DirectSound path is more reliable than WASAPI on macOS;
  for SDL-based games prefer `SDL_AUDIODRIVER=directsound` (note some engines
  override this with their own cvar).
- **Performance**: GL-heavy renderers pay a per-call cost through Wine's
  PE→Unix boundary under Rosetta; software renderers presenting a single
  texture are often dramatically faster.
- **Direct3D renderer**: this fork defaults wined3d to the Vulkan renderer
  (`HKCU\Software\Wine\Direct3D` `renderer=vulkan`, installed by wine.inf;
  delete or override per-app to go back to GL). The winemac GL renderer is
  capped at GL 2.1 compatibility contexts and cannot present to
  cross-process windows; wined3d-on-MoltenVK presents through Metal layers
  everywhere, including other processes' top-level windows (exported CAContext
  remote layers hosted by the owner via CALayerHost).
- **Electron/Chromium apps** work out of the box with two fork changes:
  1. With the Vulkan renderer above, ANGLE's D3D11 backend initializes
     cleanly in the GPU process (no GPU-process crash/fallback loop).
  2. Chromium's viz compositor runs in the GPU *process* and blits
     software-composited frames into a window owned by the browser process,
     which stock winemac cannot show (no server-side drawables on macOS,
     unlike X11). This fork adds cross-process window surfaces in win32u —
     drawing to another process's window lands in a shared-memory surface
     whose flush notifies the owner to blit into the real window surface.

  Note many Electron apps (e.g. ones calling
  `app.disableHardwareAcceleration()`) choose software compositing
  themselves; the cross-process surface path is what puts those frames on
  screen.
- First prefix creation shows Mono/Gecko installer dialogs; for headless use
  set `WINEDLLOVERRIDES="mscoree,mshtml="`.
