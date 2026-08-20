// Worker host: load the emscripten bundle. The module auto-runs main(), which
// boots wine + the interpreter and (on WEBWINE_BROWSER) posts 'frame'/'log'
// messages to the main thread via webwine_present()/Module.print.
importScripts('webwine-bw.js');
