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
  // Keep the bootstrap observable: importScripts() blocks this worker while
  // the data package and wasm are initialized, and a failure before the
  // module's own callbacks otherwise looks like an infinite black canvas.
  try {
    postMessage({ type: 'log', line: 'worker: input received; loading webwine-bw.js' });
    /* A stale worker/module pair presents as a black canvas with no useful
       browser error.  The static server already sends no-store, but some
       browser/proxy cache layers still reuse an old imported script.  Keep the
       bootstrap URL unique; Emscripten strips this query when resolving the
       adjacent wasm/data files. */
    /* Emscripten normally fetches the adjacent wasm/data files with their
       plain names.  Pair those requests with the worker too: otherwise a
       browser that retained an older payload can combine it with this
       worker and leave the page at a black canvas. */
    var boot = Date.now();
    self.Module = self.Module || {};
    self.Module.locateFile = function (name, prefix) {
      return (prefix || '') + name + '?boot=' + boot;
    };
    importScripts('webwine-bw.js?boot=' + boot);
  } catch (e) {
    postMessage({ type: 'log', line: 'FATAL worker bootstrap: ' + (e && e.stack || e) });
    throw e;
  }
};
