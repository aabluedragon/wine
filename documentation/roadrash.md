# Road Rash: 30th Anniversary Edition

Tested with `/home/alon/games/RoadRash/RoadRash95.exe` on Linux using this
fork's existing WoW64 build in `/home/alon/dev/wine-build`.

## Fixes

The game needs the 32-bit installation registry entries from its
`installScript.vdf`: `HKLM\Software\Electronic Arts\RoadRash 95`, with `Path`
pointing to the game directory and `Language` set to `English`.

Media Foundation playback otherwise loops while the color converter requests
an EVR allocator sample's contiguous buffer. Allocator samples now forward
buffer conversion, length and copying to their underlying sample. The legacy
`MFCreateVideoSampleFromSurface` behavior remains separate.

The GStreamer MF source also needs to repack I420/YV12 video planes. GStreamer
aligns each plane's rows to four bytes; MF expects tightly packed planar data.
The 1458-pixel-wide intro exposed the mismatch as diagonal image corruption.
The MF parser flag leaves DirectShow and WM reader layouts unchanged.

## Running locally

Run `/home/alon/games/RoadRash/run-wine.sh`, or select **Road Rash (Wine fork)**
in the applications menu. The launcher uses the patched fork installed under
`/home/alon/.local`, the prefix at `/mnt/storage/roadrash-wine/prefix`, and a
1024x768 Wine desktop window. Set `WINEPREFIX` to choose another prefix.

Keyboard defaults are W/S for accelerate/brake and A/D for steering. Select
**Restroom > Configure Input > Keyboard defaults** if controller mappings are
selected. Escape skips video, and pressing it twice during a race quits the
race. Controller mappings can be selected from the same input menu.

## Building and verification

From the configured build directory:

```sh
make -j4 dlls/evr/i386-windows/evr.dll dlls/evr/x86_64-windows/evr.dll \
  dlls/winegstreamer/winegstreamer.so \
  dlls/winegstreamer/i386-windows/winegstreamer.dll \
  dlls/winegstreamer/x86_64-windows/winegstreamer.dll
```

With tests enabled, build `dlls/evr/tests/{i386,x86_64}-windows/evr_test.exe`
and run each executable with the argument `allocator`. Both architectures
pass 22 checks. The unpatched installed build fails the three checks for
contiguous conversion, total length and copying.

The wider `evr` test has an existing presenter-orientation image comparison
failure (63% difference) and subsequent access violation on this environment;
the same failures reproduce with the unpatched installed build.

Runtime checks covered intro and prerace video playback, menus, a race with
W-key acceleration to approximately 100 MPH, and nonzero game audio captured
through PipeWire. Logs, screenshots and original installed libraries are in
`/mnt/storage/roadrash-wine/`.
