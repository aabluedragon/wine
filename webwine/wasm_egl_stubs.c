/*
 * The EGL entry points emscripten does not implement.
 *
 * win32u's EGL backend (dlls/win32u/opengl.c) resolves all 44 functions in
 * ALL_EGL_FUNCS and refuses to initialise if any one is missing.  Emscripten's
 * libEGL provides 26 of them; the other 18 are for features that cannot exist
 * in a browser anyway - EGLImage, fence sync objects, pbuffers, pixmaps, and
 * the EGL 1.5 platform-display variants.  Wine only *calls* those once the
 * corresponding extension is advertised, which it never is here, so failing
 * cleanly is enough to let initialisation get past the symbol check.
 *
 * eglGetProcAddress is NOT here: emscripten does provide it, but only when the
 * link includes -sGL_ENABLE_GET_PROC_ADDRESS (otherwise it is a stub that
 * aborts, and the linker tells you so).  See browser/build-browser.sh.
 */
#include <EGL/egl.h>

static EGLBoolean fail( void ) { return EGL_FALSE; }

EGLBoolean EGLAPIENTRY eglBindTexImage( EGLDisplay d, EGLSurface s, EGLint b ) { return fail(); }
EGLBoolean EGLAPIENTRY eglReleaseTexImage( EGLDisplay d, EGLSurface s, EGLint b ) { return fail(); }
EGLBoolean EGLAPIENTRY eglCopyBuffers( EGLDisplay d, EGLSurface s, EGLNativePixmapType t ) { return fail(); }
EGLBoolean EGLAPIENTRY eglSurfaceAttrib( EGLDisplay d, EGLSurface s, EGLint a, EGLint v ) { return fail(); }

/* sync objects */
EGLSync EGLAPIENTRY eglCreateSync( EGLDisplay d, EGLenum t, const EGLAttrib *a ) { return EGL_NO_SYNC; }
EGLBoolean EGLAPIENTRY eglDestroySync( EGLDisplay d, EGLSync s ) { return fail(); }
EGLint EGLAPIENTRY eglClientWaitSync( EGLDisplay d, EGLSync s, EGLint f, EGLTime t ) { return EGL_FALSE; }
EGLBoolean EGLAPIENTRY eglGetSyncAttrib( EGLDisplay d, EGLSync s, EGLint a, EGLAttrib *v ) { return fail(); }
EGLBoolean EGLAPIENTRY eglWaitSync( EGLDisplay d, EGLSync s, EGLint f ) { return fail(); }

/* EGLImage */
EGLImage EGLAPIENTRY eglCreateImage( EGLDisplay d, EGLContext c, EGLenum t, EGLClientBuffer b, const EGLAttrib *a )
{ return EGL_NO_IMAGE; }
EGLBoolean EGLAPIENTRY eglDestroyImage( EGLDisplay d, EGLImage i ) { return fail(); }

/* pbuffers and pixmaps: there is no such drawable in a browser */
EGLSurface EGLAPIENTRY eglCreatePbufferSurface( EGLDisplay d, EGLConfig c, const EGLint *a ) { return EGL_NO_SURFACE; }
EGLSurface EGLAPIENTRY eglCreatePbufferFromClientBuffer( EGLDisplay d, EGLenum t, EGLClientBuffer b,
                                                         EGLConfig c, const EGLint *a ) { return EGL_NO_SURFACE; }
EGLSurface EGLAPIENTRY eglCreatePixmapSurface( EGLDisplay d, EGLConfig c, EGLNativePixmapType p, const EGLint *a )
{ return EGL_NO_SURFACE; }
EGLSurface EGLAPIENTRY eglCreatePlatformPixmapSurface( EGLDisplay d, EGLConfig c, void *p, const EGLAttrib *a )
{ return EGL_NO_SURFACE; }

/* EGL 1.5 platform displays: emscripten only knows EGL_DEFAULT_DISPLAY */
EGLDisplay EGLAPIENTRY eglGetPlatformDisplay( EGLenum platform, void *native, const EGLAttrib *a )
{ return EGL_NO_DISPLAY; }
EGLSurface EGLAPIENTRY eglCreatePlatformWindowSurface( EGLDisplay d, EGLConfig c, void *win, const EGLAttrib *a )
{ return EGL_NO_SURFACE; }
