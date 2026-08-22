// --pre-js for the browser (worker) bundle: env, FS symlinks, argv, log relay.
// Runs inside the emscripten module scope before the runtime starts.
Module['arguments'] = ['c:\\netduke32.exe'];

/* The EGL/WebGL backend needs a canvas to make a context from, and a worker has
   no DOM.  An OffscreenCanvas made HERE (not one transferred from the page) is
   the right kind: a transferred canvas only composites when its task yields, and
   this worker is inside the interpreter for the whole session and never does -
   whereas transferToImageBitmap() on a worker-owned one hands the finished
   picture straight to the page (see webwine_gl_present in ../wasm_ipc.c). */
try {
  Module['canvas'] = new OffscreenCanvas(640, 400);
  /* Make the context HERE rather than leaving it to emscripten's EGL, because
     the one attribute that matters is not reachable through EGL:
     preserveDrawingBuffer.  transferToImageBitmap() resets the drawing buffer,
     and the engine does not redraw every pixel of every frame - once it settles
     on a static screen it stops drawing entirely and what we captured would be
     the reset value, a transparent frame.  A second getContext() returns this
     same context and ignores its attributes, so emscripten picks this one up. */
  Module['canvas'].getContext('webgl2', { alpha: false, depth: true, stencil: true,
                                          antialias: false, preserveDrawingBuffer: true });
} catch (e) {}

Module['print'] = function (t) { try { postMessage({ type: 'log', line: t }); } catch (e) {} };
Module['printErr'] = function (t) { try { postMessage({ type: 'log', line: t }); } catch (e) {} };

Module['preRun'] = Module['preRun'] || [];
Module['preRun'].push(function () {
  var E = ENV;
  E.WINEPREFIX = '/prefix';
  E.WINEDLLPATH = '/root/lib/wine';
  E.WINEUNIXDIR = '/root/lib/wine/i386-unix';
  E.WINEDATADIR = '/root/share/wine';
  E.WINE_START_CWD = '/game';
  E.WINELOADERNOEXEC = '1';
  E.WINE_NO_SERVER_SPAWN = '1';
  E.WINE_AUTO_ENTER = '1';
  /* 'windows' (not 'dummy'): SDL then creates a real Win32 window through our
     null user driver and pumps WM_* messages, which is what makes keyboard and
     mouse work at all - and it measured faster than the dummy driver too. */
  E.SDL_VIDEODRIVER = 'windows';
  /* 'dummy' just has to let SDL_InitSubSystem(AUDIO) succeed - we intercept
     SDL_OpenAudioDevice itself and drive the game's callback ourselves,
     because opening a real device would need an audio thread we do not have. */
  E.SDL_AUDIODRIVER = 'dummy';

  /* Anything uppercase in the page URL's query string lands in the guest env
     (?WASM_PROF=1, ?WASM_NO_MOUSE=1, ...).  Lets one built bundle be A/B'd
     without a rebuild, which is the only way to compare fairly on a machine
     whose background load drifts between builds. */
  try { var X = self.__wwEnv || {}; for (var k in X) E[k] = X[k]; } catch (e) {}

  /* ?WW_ARGS=a,b,c appends command line arguments for the game, so a session can
     start where you want it (-map E1L1.MAP) instead of being driven through the
     menus - booting takes long enough that it matters when iterating. */
  if (E.WW_ARGS) Module['arguments'] = Module['arguments'].concat(E.WW_ARGS.split(','));

  function mkdirp(p) { try { FS.mkdirTree ? FS.mkdirTree(p) : FS.mkdir(p); } catch (e) {} }
  function sym(t, l) { try { FS.symlink(t, l); } catch (e) {} }
  mkdirp('/prefix/dosdevices');
  sym('/game', '/prefix/dosdevices/c:');
  sym('/', '/prefix/dosdevices/z:');
  // guest loader resolves C:\windows\system32 for the PE builtins
  mkdirp('/game/windows');
  sym('/root/lib/wine/i386-windows', '/game/windows/system32');
});

/* ?WW_GL=1 puts the game on its OpenGL (Polymost) renderer: 32 bits per pixel is
   what selects it, and the classic renderer's 1/N upscale does not apply.
   Rewrite those two lines rather than appending more - a second vidmode line
   makes the engine set the software mode afterwards and tear the GL window back
   down - and everything else in the config stays identical between the arms.

   This has to run here, not in preRun: the --preload-file data is unpacked by a
   preRun callback of the packager's own, which is queued after ours, so the game
   files do not exist yet while preRun is running. */
Module['onRuntimeInitialized'] = function () {
  try {
    if (!(self.__wwEnv || {}).WW_GL) return;
    var f = '/game/autoexec.cfg';
    var cfg = FS.readFile(f, { encoding: 'utf8' })
                .replace(/^vidmode .*$/m, 'vidmode 640 400 32 0')
                .replace(/^r_upscalefactor .*$/m, 'r_upscalefactor 1');
    /* ?WW_NOIDX=1 turns off the engine's indexed-colour texture path, which is
       what packs tiles into one big atlas and addresses them from uniforms. */
    if ((self.__wwEnv || {}).WW_NOIDX) cfg += 'r_useindexedcolortextures 0\n';
    FS.writeFile(f, cfg);
  } catch (e) { err('wasm_x86: GL autoexec.cfg failed: ' + e); }
};
