// Worker host: load the emscripten bundle. The module auto-runs main(), which
// boots wine + the interpreter and posts 'frame'/'log' messages to the main
// thread via webwine_present()/Module.print.
//
// The page sends the input SharedArrayBuffer ring first; we stash it and only
// THEN load the module, because once the interpreter starts this thread never
// returns to its event loop and can never receive another message.
self.onmessage = function (e) {
  if (!e.data || e.data.type !== 'input') return;
  if (e.data.ring) self.__wwInput = e.data.ring;   // Int32Array over a SharedArrayBuffer
  self.onmessage = null;
  importScripts('webwine-bw.js');
};
