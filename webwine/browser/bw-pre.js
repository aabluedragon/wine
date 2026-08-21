// --pre-js for the browser (worker) bundle: env, FS symlinks, argv, log relay.
// Runs inside the emscripten module scope before the runtime starts.
Module['arguments'] = ['c:\\netduke32.exe'];

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

  function mkdirp(p) { try { FS.mkdirTree ? FS.mkdirTree(p) : FS.mkdir(p); } catch (e) {} }
  function sym(t, l) { try { FS.symlink(t, l); } catch (e) {} }
  mkdirp('/prefix/dosdevices');
  sym('/game', '/prefix/dosdevices/c:');
  sym('/', '/prefix/dosdevices/z:');
  // guest loader resolves C:\windows\system32 for the PE builtins
  mkdirp('/game/windows');
  sym('/root/lib/wine/i386-windows', '/game/windows/system32');
});
