// Worker host: load the emscripten bundle. The module auto-runs main(), which
// boots wine + the interpreter and posts 'frame'/'log' messages to the main
// thread via webwine_present()/Module.print.

/* An exception thrown out of the wasm module (a JS TypeError inside one of
   emscripten's GL entry points, say) unwinds the whole interpreter and arrives
   here as an UNHANDLED REJECTION, not an error event - so the page's
   worker.onerror never fires and a dead worker looks exactly like a slow one.
   Relay it as a log line instead; by the time this runs the stack is unwound,
   so the worker's event loop is free again. */
self.addEventListener('unhandledrejection', function (e) {
  var r = e.reason;
  try { postMessage({ type: 'log', line: 'FATAL worker exception: ' + ((r && r.message) || r) }); } catch (x) {}
});
self.addEventListener('error', function (e) {
  try { postMessage({ type: 'log', line: 'FATAL worker error: ' + (e.message || e) }); } catch (x) {}
});

// The page sends the input SharedArrayBuffer ring first; we stash it and only
// THEN load the module, because once the interpreter starts this thread never
// returns to its event loop and can never receive another message.
self.onmessage = function (e) {
  if (!e.data || e.data.type !== 'input') return;
  if (e.data.env) self.__wwEnv = e.data.env;   // uppercase query-string params -> guest env
  if (e.data.ring) self.__wwInput = e.data.ring;   // Int32Array over a SharedArrayBuffer
  if (e.data.ctl) self.__wwCtl = new Int32Array(e.data.ctl);   // watchdog + present back-pressure
  if (e.data.audio) {                              // PCM ring drained by the AudioWorklet
    self.__wwAudio = {
      idx:  new Int32Array(e.data.audio, 0, 2),
      data: new Float32Array(e.data.audio, 8),
      cap:  e.data.audioCap,
    };
  }
  self.onmessage = null;
  importScripts('webwine-bw.js');
};
