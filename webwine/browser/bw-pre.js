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
  E.SDL_VIDEODRIVER = 'windows';   /* EXPERIMENT: real Win32 window -> WM_* input */
  E.SDL_AUDIODRIVER = 'dummy';

  function mkdirp(p) { try { FS.mkdirTree ? FS.mkdirTree(p) : FS.mkdir(p); } catch (e) {} }
  function sym(t, l) { try { FS.symlink(t, l); } catch (e) {} }
  mkdirp('/prefix/dosdevices');
  sym('/game', '/prefix/dosdevices/c:');
  sym('/', '/prefix/dosdevices/z:');
  // guest loader resolves C:\windows\system32 for the PE builtins
  mkdirp('/game/windows');
  sym('/root/lib/wine/i386-windows', '/game/windows/system32');
});
