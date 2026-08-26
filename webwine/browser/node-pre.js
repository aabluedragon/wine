// Node has no DOM.  netduke32 probes GL at startup regardless of the renderer
// (ScreenBPP), so emscripten's eglCreateContext calls Browser.getCanvas()
// .getContext(...).  With no canvas that throws and kills the run.  Give it a
// stub canvas whose getContext returns null: createContext then returns 0,
// eglCreateContext returns EGL_NO_CONTEXT, and the game falls back to its
// classic software renderer (which is all node can measure anyway).
Module['canvas'] = {
  getContext: function () { return null; },
  addEventListener: function () {},
  removeEventListener: function () {},
  width: 0, height: 0, style: {},
};
