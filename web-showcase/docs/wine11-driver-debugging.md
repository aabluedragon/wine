# Packaged Wine 11.0 OpenGL module provenance and debug rebuild

This document prevents a future session from debugging or replacing the wrong
Wine module. The immediate blocker is now later than GLX make-current and the
extension-count thunk, so a driver rebuild is not the first next action; use
this procedure when matched Wine-native instrumentation is needed.

## Which module actually runs

Inside the canonical root ZIP:

| Path | Kind | Role |
| --- | --- | --- |
| `/opt/wine/lib/wine/i386-unix/winex11.so` | ELF32 i386, 442,840 bytes | Actual Wine X11 Unix driver implementation. |
| `/opt/wine/lib/wine/i386-unix/opengl32.so` | ELF32 i386, stripped | Wine OpenGL Unix wrappers; contains the GL 2 extension-count branch used by the `gl2ext` diagnostic. |
| `/opt/wine/lib/wine/i386-unix/winex11.drv.so` | ELF32 i386, about 84 KiB | Legacy loader wrapper; not the implementation targeted for X11 GL instrumentation. |
| `/opt/wine/lib/wine/i386-windows/winex11.drv` | PE32, 2,560 bytes | Windows fake/stub module. |
| `/home/username/.wine/drive_c/windows/system32/winex11.drv` | PE32, 2,560 bytes | Prefix copy of the fake/stub module. |

For source-level X11 OpenGL instrumentation, the target is
`i386-unix/winex11.so`.

## Exact packaged identity

Canonical root:

```text
web-showcase/build-gl/tinycore-wine11-parent-inline-webgl-pci-glxshim.zip
SHA-256 47e1d4d0961c62693960a756e8179ed2497a7784583c904de240c4d24e0c15fe
```

Native driver in that root:

```text
SHA-256 7fa3fe52a8d9c515b32bda094de96bb859ff7ecf89970f00dba946dcb184886d
Build ID 52876716255ba53147f3e3724cd73aee658a09ea
```

The unmodified downloaded Wine 11 root contains a driver with SHA-256
`ee3cdda82f7d4d059aeaa105fc47c2317fb04ff2c10d38b3e7878de46452df88`
and the same build ID. Binary comparison shows the parent driver differs by one
data byte at one-indexed offset 441137 (file offset `0x6bb30`), from `1` to `0`.
That is consistent with forcing initialized `BOOL use_egl` from true to false.

The build ID is stale after the binary data patch and cannot distinguish these
drivers. Use the SHA-256 and root identity.

Useful checks:

```sh
VIBE_ROOT_ZIP=web-showcase/build-gl/tinycore-wine11-parent-inline-webgl-pci-glxshim.zip
unzip -p "$VIBE_ROOT_ZIP" wineVersion.txt
unzip -p "$VIBE_ROOT_ZIP" build.txt
unzip -p "$VIBE_ROOT_ZIP" opt/wine/lib/wine/i386-unix/winex11.so | file -
unzip -p "$VIBE_ROOT_ZIP" opt/wine/lib/wine/i386-unix/winex11.so | shasum -a 256
```

Use a task-specific variable such as `VIBE_ROOT_ZIP`; do not repurpose shell
environment variables like `HOME`.

## Exact Wine 11.0 `opengl32.so` diagnostic

The canonical root's Unix OpenGL wrapper is:

```text
/opt/wine/lib/wine/i386-unix/opengl32.so
SHA-256 60bb64157afc62067ee2298fac0a96943deaf63783efd30c91b8ad8c305eaea0
```

Wine 11.0 `dlls/opengl32/unix_wgl.c:get_integer()` synthesizes
`GL_NUM_EXTENSIONS` from `ctx->extension_count` before calling the driver. In
the GL 2 WebGL bridge, that produced 7 while `glGetStringi` was correctly
unavailable. The application then called a NULL indexed-string slot.

The script
`web-showcase/tools/patch-wine11-gl2-extension-count.mjs` is pinned to the
canonical module's full hash and surrounding machine code. It changes the
conditional branch at file offsets `0x6aac0`-`0x6aac1` from `74 65` to
`90 90`, allowing enum `0x821d` to follow the normal driver path. Output:

```text
SHA-256 a3dceaa1f93019a76da43c9b5019c9847b25abc385af2e20c6da47821649287b
```

That module is preserved only in the derivative root:

```text
web-showcase/build-gl/tinycore-wine11-parent-inline-webgl-pci-glxshim-gl2ext.zip
SHA-256 1bdc1c5b03f755ecd1aedef491f5fecfc2e2a18fe0f1d1e14f056e12810683cc
```

Live output then contains `GLX: GL2 GL_NUM_EXTENSIONS -> 0` and advances to
`glShadeModel`, proving the dispatch diagnosis. This is a narrow, matched
Wine 11.0 binary diagnostic; it does not make current-tree ABI-38 modules safe
to overlay. Preserve the canonical root as a regression control. The exact
non-destructive repack procedure is in [reproduction.md](reproduction.md).

## Packaged source provenance

The ZIP reports Wine `11.0`. The peeled upstream `wine-11.0` tag is:

```text
db11d0fe6a169c457e23d007e20404643d067aa8
```

Its embedded `build.txt` records:

```text
git checkout wine-11.0
git apply ../patches/fixSetupApiFromCrashingDuringDllDetach.patch
git apply ../patches/FAudio_opentdd_mac_crash.patch
./configure LDFLAGS="-s" CFLAGS="-O2 -msse2 -march=pentium4 -mfpmath=sse $EXTRA" --without-cups --without-pulse --without-dbus --without-sane --without-hal --without-udev --without-usb --without-xshape --without-xshm --without-ldap --without-alsa --prefix=/opt/wine --disable-tests $EXTRA_ARGS
make
```

The binary contains GCC's Debian `10.2.1-6` compiler string. It is stripped:
there is no `.symtab`, `.strtab`, GNU debug link, separate `.debug/.dbg/.dwo`,
or `/usr/lib/debug` companion in the current roots. It retains `.dynsym` and
`.eh_frame` only.

The local Wine checkout is newer and currently defines OpenGL driver ABI 38.
Wine 11.0 defines ABI 37, with different `opengl_funcs` and
`opengl_driver_funcs` layouts. Never drop the local checkout's `winex11.so` or
`win32u.so` into this filesystem.

## Guest GL shim boundary symbols

The root's custom `/lib/libGL.so.1.2.0` is unstripped, SHA-256
`f09030c9fc8cbfa71e04985629217159aa61332363d014e12d39d993ea850c2f`,
build ID `b124d9adf8241f669ab7725fb56bc52a1e885903`. Useful symbols include:

```text
glGetIntegerv          0x204ed  (int 0x99 opcode 0x20)
glGetStringi           0x23301  (int 0x99 opcode 0xb50)
glXMakeCurrent         0x3bf50
glXMakeContextCurrent  0x3c1ed
glXGetProcAddressARB   0x3c302
```

Both make-current entry points route through the BoxedWine int-0x99 GLX bridge;
`glXMakeContextCurrent` adapts its four arguments to the three-argument
make-current handler. BoxedWine logs `GLX: glXMakeCurrent` on handler entry.

Do not copy `/Users/alonamir/dev/boxedwine/tools/opengl/libGL.so.1` over this
file. That sibling binary has different code, returns NULL from its resolver,
and uses an obsolete opcode for `glXMakeContextCurrent`. Rebuild the current
guest shim source as Linux i386 when replacement is necessary.

## Rebuilding a matched debug Wine

The sibling BoxedWine checkout contains a purpose-built builder:

```text
/Users/alonamir/dev/boxedwine/tools/buildWine/build_wine.py
/Users/alonamir/dev/boxedwine/tools/buildWine/wine_builds.json
/Users/alonamir/dev/boxedwine/tools/buildWine/README.md
```

Use Debian 11 on amd64 with i386 development packages, or an amd64 container/VM
with enough memory. The current host is arm64 macOS and cannot natively produce
the required i386 Linux ELF. The available Podman VM was stopped and configured
with only 2 GiB during the investigation; increase resources or use native
amd64 Linux.

From the builder directory, first copy `wine_builds.json` to a debug config.
In that copy, keep the `wine-11.0` tag and operations, remove stripping from
`ldflags`, and use debug C flags equivalent to:

```text
-g2 -O2 -msse2 -march=pentium4 -mfpmath=sse
```

Then run:

```sh
python3 build_wine.py wine-11.0 \
  --config wine_builds-debug.json \
  --jobs 12 \
  --skip-wineboot
```

The builder's current config includes more feature checks than the historical
package. Preserve the Wine 11.0 tag and verify the resulting ABI and linked
libraries before overlaying anything.

If only X11 OpenGL instrumentation is needed after configure, instrument Wine
11.0 `dlls/winex11.drv/opengl.c`, especially `x11drv_make_current()`, then
rebuild the `dlls/winex11.drv/winex11.so` target. Useful logging boundaries are:

- entry and the raw draw/read/context arguments;
- before dereferencing draw/read private data;
- immediately before and after `pglXMakeCurrent` or
  `pglXMakeContextCurrent`;
- returned boolean and X/GL error state.

Overlay only the matched Wine 11.0 file at:

```text
/opt/wine/lib/wine/i386-unix/winex11.so
```

Keep the previous root ZIP unchanged, create a clearly named derivative, and
record both outer ZIP and inner module hashes. Validate that `wineVersion.txt`,
ABI 37, and the expected GLX handler logs remain intact before using the result
to draw conclusions.

## Historical evidence commands

On a shell that supports process substitution, the one-byte comparison can be
repeated with:

```sh
cmp -l \
  <(unzip -p web-showcase/build-gl/tinycore-wine11.zip \
      opt/wine/lib/wine/i386-unix/winex11.so) \
  <(unzip -p web-showcase/build-gl/tinycore-wine11-parent-inline-webgl-pci-glxshim.zip \
      opt/wine/lib/wine/i386-unix/winex11.so)
```

Extract to a temporary directory before using tools that cannot read process
substitution, then remove only that explicit temporary directory.
