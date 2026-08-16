# Fork working instructions

## Strict repository and executable boundary

Only edit source files inside `/Users/alonamir/dev/wine`. Do not edit, build,
reconfigure, clean, reset, or otherwise experiment with source in sibling
projects such as `/Users/alonamir/dev/eduke32` or
`/Users/alonamir/dev/boxedwine` unless the user explicitly asks for that exact
sibling-project action. A broad request to fix or continue the Wine task is
not permission to modify another project.

For the browser task, run only the user-supplied executable
`/Users/alonamir/games/netduke32_v1.2.1/netduke32.exe` (PE32 i386, SHA-256
`547dea93d40114dee7757a049f20e0f7659cbd0c221ae9cf4258338e94c33878`)
through this Wine project. Do not substitute or build an executable from the
EDuke32/VibeBuild32 source checkout.

Keep this Wine repository on its `vibe` branch. The sibling BoxedWine checkout
is currently on `master`, and the sibling EDuke32 checkout is on `vibe`;
preserve both in place rather than switching them. Do not switch branches,
reset, clean, or stash any of the three working trees unless the user
explicitly asks. They intentionally contain unfinished and untracked work from
the macOS, Android, iOS, and WASM integration.

The consolidated platform history is already on `vibe` (marker commit
`67591003fffa8a2a21f3ca73c45a2bcdfde2279c`); do not recreate or re-merge the
old platform branches. Read `BUILDING-MACOS.md` before native Apple build work.

For the browser/VibeBuild32 task, read these files before making changes:

1. `web-showcase/docs/README.md`
2. `web-showcase/docs/current-state.md`
3. `web-showcase/docs/reproduction.md`
4. `web-showcase/docs/wasm-opengl-blocker.md`
5. `web-showcase/docs/wine11-driver-debugging.md`

The sibling VibeBuild32/EDuke32 and BoxedWine sources are out of scope unless
the user grants explicit permission for a named change there.

The browser objective is not complete until the packaged 32-bit Windows build
renders a real 32-bpp OpenGL frame through Wine and BoxedWine/WebGL and accepts
input. A boot log, GL context creation, or an all-black canvas is not success.

The current preserved diagnostic uses a pthread, interpreter-only BoxedWine
`Build/Control` runtime and an explicit Wine 11.0 GLX-shim root ZIP. The old
make-current fault, NULL `glGetStringi`, and two optional startup NULL calls
(`glBindSampler(0,0)` and `glIsSync(NULL)`) are cleared by Wine-repository
diagnostic derivatives. The latest client-state derivative reaches a real
640x480 canvas and produces a non-black startup frame, but then tears the
context down; sustained rendering and input are not yet proven. Its current
guest-GL blocker is the broader legacy fixed-function/WebGL1 boundary, not a
source change in the supplied executable. All such patches are binary
diagnostics under `web-showcase/tools/` and must remain inside this Wine repo.
Bare `/` on the development server selects a different root, and `make runtime`
builds a different JIT configuration. Always use the artifact hashes and
explicit URL from `web-showcase/docs/current-state.md` before comparing
results.

After every material browser test, update `current-state.md` with the local
time, commits, dirty-source warning, artifact hashes, exact URL, decisive log
lines, and canvas result. Keep observations separate from hypotheses.

When the user says “continue” or “fix the next blocker,” resume the numbered
immediate action in `current-state.md`. Do not replay cleared failures unless
using the named canonical root as an explicit regression control.
