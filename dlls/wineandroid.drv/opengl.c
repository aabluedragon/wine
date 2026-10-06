/*
 * Android OpenGL functions
 *
 * Copyright 2000 Lionel Ulmer
 * Copyright 2005 Alex Woods
 * Copyright 2005 Raphael Junqueira
 * Copyright 2006-2009 Roderick Colenbrander
 * Copyright 2006 Tomas Carnecky
 * Copyright 2013 Matteo Bruni
 * Copyright 2012, 2013, 2014, 2017 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

#include "ntstatus.h"
#include "android.h"
#include "winternl.h"

#include "wine/opengl_driver.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(android);

#ifndef min
#define min(a,b) ((a) < (b) ? (a) : (b))
#endif

static const struct egl_platform *egl;
static const struct opengl_funcs *funcs;
static const struct opengl_drawable_funcs android_drawable_funcs;

struct gl_drawable
{
    struct opengl_drawable base;
    ANativeWindow  *window;
    UINT            width;      /* pbuffer size */
    UINT            height;
    void           *readback;   /* scratch buffer for presenting frames */
};

static struct gl_drawable *impl_from_opengl_drawable( struct opengl_drawable *base )
{
    return CONTAINING_RECORD( base, struct gl_drawable, base );
}

static void *opengl_handle;

static EGLConfig egl_config_for_format(int format)
{
    return egl->configs[(format - 1) % egl->config_count];
}

static void android_drawable_destroy( struct opengl_drawable *base )
{
    struct gl_drawable *gl = impl_from_opengl_drawable( base );
    free( gl->readback );
    release_ioctl_window( gl->window );
}

void update_gl_drawable( HWND hwnd )
{
    /* clear any cached opengl drawable */
    set_window_opengl_drawable( hwnd, NULL, TRUE );
    set_window_opengl_drawable( hwnd, NULL, FALSE );
    NtUserRedrawWindow( hwnd, NULL, 0, RDW_INVALIDATE | RDW_ERASE );
}

static EGLSurface create_pbuffer_surface( EGLConfig config, int width, int height )
{
    const int attribs[] = { EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE };
    return funcs->p_eglCreatePbufferSurface( egl->display, config, attribs );
}

/* Rendering always goes to a pbuffer here. The EGL implementation on current
 * Android releases refuses our proxied ANativeWindow as a native window - its
 * buffers live in another process - so an EGL window surface cannot work.
 * Instead every frame is read back and copied into one of the window's own
 * gralloc buffers through the same LOCK/UNLOCK_AND_POST path that the software
 * window surfaces already present through. */
static BOOL android_surface_create( struct client_surface *client, int format, struct opengl_drawable **drawable )
{
    struct gl_drawable *gl;
    EGLConfig config = egl_config_for_format( format );
    int w = 0, h = 0;

    TRACE( "hwnd %p, format %d, drawable %p\n", client->hwnd, format, drawable );

    if (*drawable)
    {
        gl = impl_from_opengl_drawable( *drawable );
        gl->base.format = format;
        TRACE( "Updated drawable %s\n", debugstr_opengl_drawable( *drawable ) );
        return TRUE;
    }

    if (!(gl = opengl_drawable_create( sizeof(*gl), &android_drawable_funcs, format, client ))) return FALSE;
    gl->window = get_client_window( client->hwnd );

    if (gl->window->query( gl->window, NATIVE_WINDOW_WIDTH, &w ) || w <= 0) w = 0;
    if (gl->window->query( gl->window, NATIVE_WINDOW_HEIGHT, &h ) || h <= 0) h = 0;
    if (!w || !h) w = h = 1;  /* the Java side has not created the view yet, resized at swap time */

    if (!(gl->base.surface = create_pbuffer_surface( config, w, h )))
    {
        WARN( "Failed to create a %dx%d pbuffer for hwnd %p\n", w, h, client->hwnd );
        opengl_drawable_release( &gl->base );
        return FALSE;
    }
    gl->width = w;
    gl->height = h;

    TRACE( "Created drawable %s %ux%u with client window %p\n",
           debugstr_opengl_drawable( &gl->base ), gl->width, gl->height, gl->window );
    *drawable = &gl->base;
    return TRUE;
}

static void android_init_egl_platform( struct egl_platform *platform )
{
    platform->type = EGL_PLATFORM_ANDROID_KHR;
    platform->native_display = EGL_DEFAULT_DISPLAY;
    egl = platform;
}

static void *android_get_proc_address( const char *name )
{
    void *ptr;
    if ((ptr = dlsym( opengl_handle, name ))) return ptr;
    return funcs->p_eglGetProcAddress( name );
}

/* resize the pbuffer to track the window, keeping it current if it was */
static void android_drawable_resize( struct gl_drawable *gl, int width, int height )
{
    EGLSurface surface;

    if (!(surface = create_pbuffer_surface( egl_config_for_format( gl->base.format ), width, height )))
    {
        WARN( "Failed to resize pbuffer to %dx%d for hwnd %p\n", width, height, gl->base.client->hwnd );
        return;
    }

    if (funcs->p_eglGetCurrentSurface( EGL_DRAW ) == gl->base.surface ||
        funcs->p_eglGetCurrentSurface( EGL_READ ) == gl->base.surface)
        funcs->p_eglMakeCurrent( egl->display, surface, surface, funcs->p_eglGetCurrentContext() );

    funcs->p_eglDestroySurface( egl->display, gl->base.surface );
    gl->base.surface = surface;
    gl->width = width;
    gl->height = height;
    free( gl->readback );
    gl->readback = NULL;
    TRACE( "Resized drawable %s to %dx%d\n", debugstr_opengl_drawable( &gl->base ), width, height );
}

/* copy the rendered frame into one of the window's gralloc buffers */
static void android_drawable_present( struct gl_drawable *gl )
{
    ANativeWindow_Buffer buffer;
    ARect rc;
    GLint fbo = 0, pbo = 0, align = 4;
    UINT width = gl->width, height = gl->height, copy_w, copy_h, x, y;
    int ret;

    if (!gl->readback && !(gl->readback = malloc( (size_t)width * height * 4 ))) return;

    /* the application's pixel-path state must not leak into the readback */
    funcs->p_glGetIntegerv( GL_READ_FRAMEBUFFER_BINDING, &fbo );
    funcs->p_glGetIntegerv( GL_PIXEL_PACK_BUFFER_BINDING, &pbo );
    funcs->p_glGetIntegerv( GL_PACK_ALIGNMENT, &align );
    if (fbo) funcs->p_glBindFramebuffer( GL_READ_FRAMEBUFFER, 0 );
    if (pbo) funcs->p_glBindBuffer( GL_PIXEL_PACK_BUFFER, 0 );
    if (align != 4) funcs->p_glPixelStorei( GL_PACK_ALIGNMENT, 4 );

    while (funcs->p_glGetError() != GL_NO_ERROR) /* drain */;
    funcs->p_glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, gl->readback );
    if (TRACE_ON(android))
    {
        GLenum err = funcs->p_glGetError();
        GLint rb = 0, db = 0;
        funcs->p_glGetIntegerv( GL_READ_BUFFER, &rb );
        funcs->p_glGetIntegerv( GL_DRAW_FRAMEBUFFER_BINDING, &db );
        if (err) TRACE( "glReadPixels error %#x read_buffer %#x draw_fbo %d\n", err, rb, db );
    }

    if (align != 4) funcs->p_glPixelStorei( GL_PACK_ALIGNMENT, align );
    if (pbo) funcs->p_glBindBuffer( GL_PIXEL_PACK_BUFFER, pbo );
    if (fbo) funcs->p_glBindFramebuffer( GL_READ_FRAMEBUFFER, fbo );

    rc.left = rc.top = 0;
    rc.right = width;
    rc.bottom = height;
    if ((ret = gl->window->perform( gl->window, NATIVE_WINDOW_LOCK, &buffer, &rc )))
    {
        WARN( "Failed to lock hwnd %p window buffer, error %d\n", gl->base.client->hwnd, ret );
        return;
    }

    copy_w = min( width, (UINT)buffer.width );
    copy_h = min( height, (UINT)buffer.height );
    for (y = 0; y < copy_h; y++)
    {
        /* GL rows are bottom-up; the window buffer is top-down */
        const unsigned int *src = (const unsigned int *)gl->readback + (size_t)(height - 1 - y) * width;
        unsigned int *dst = (unsigned int *)buffer.bits + (size_t)y * buffer.stride;
        for (x = 0; x < copy_w; x++)
        {
            unsigned int px = src[x];  /* 0xAABBGGRR from GL_RGBA */
            /* the window buffer is BGRA and composited opaque */
            dst[x] = 0xff000000 | ((px & 0x000000ff) << 16) | (px & 0x0000ff00) | ((px & 0x00ff0000) >> 16);
        }
    }
    gl->window->perform( gl->window, NATIVE_WINDOW_UNLOCK_AND_POST );
}

static BOOL android_drawable_swap( struct opengl_drawable *base )
{
    struct gl_drawable *gl = impl_from_opengl_drawable( base );
    int w = 0, h = 0;

    TRACE( "drawable %s surface %p\n", debugstr_opengl_drawable( base ), gl->base.surface );

    if (!gl->window->query( gl->window, NATIVE_WINDOW_WIDTH, &w ) &&
        !gl->window->query( gl->window, NATIVE_WINDOW_HEIGHT, &h ) &&
        w > 0 && h > 0 && ((UINT)w != gl->width || (UINT)h != gl->height))
        android_drawable_resize( gl, w, h );

    android_drawable_present( gl );
    return TRUE;
}

static void android_drawable_flush( struct opengl_drawable *base, UINT flags )
{
    TRACE( "drawable %s, flags %#x\n", debugstr_opengl_drawable( base ), flags );
}

static void android_init_extensions( struct opengl_funcs *funcs, BOOLEAN extensions[GL_EXTENSION_COUNT] )
{
    extensions[WGL_EXT_framebuffer_sRGB] = 1;
}

static struct opengl_driver_funcs android_driver_funcs =
{
    .p_init_egl_platform = android_init_egl_platform,
    .p_get_proc_address = android_get_proc_address,
    .p_init_extensions = android_init_extensions,
    .p_surface_create = android_surface_create,
};

static void android_client_surface_destroy( struct client_surface *client )
{
    TRACE( "%s\n", debugstr_client_surface( client ) );
}

static void android_client_surface_detach( struct client_surface *client )
{
}

static void android_client_surface_update( struct client_surface *client )
{
}

static void android_client_surface_present( struct client_surface *client, HDC hdc )
{
}

static const struct client_surface_funcs android_client_surface_funcs =
{
    .destroy = android_client_surface_destroy,
    .detach = android_client_surface_detach,
    .update = android_client_surface_update,
    .present = android_client_surface_present,
};

struct client_surface *ANDROID_CreateClientSurface( HWND hwnd, int pixel_format )
{
    return client_surface_create( sizeof(struct client_surface), &android_client_surface_funcs, hwnd );
}

static const struct opengl_drawable_funcs android_drawable_funcs =
{
    .destroy = android_drawable_destroy,
    .flush = android_drawable_flush,
    .swap = android_drawable_swap,
};

/**********************************************************************
 *           ANDROID_OpenGLInit
 */
UINT ANDROID_OpenGLInit( UINT version, const struct opengl_funcs *opengl_funcs, const struct opengl_driver_funcs **driver_funcs )
{
    if (version != WINE_OPENGL_DRIVER_VERSION)
    {
        ERR( "version mismatch, opengl32 wants %u but driver has %u\n", version, WINE_OPENGL_DRIVER_VERSION );
        return STATUS_INVALID_PARAMETER;
    }
    if (!opengl_funcs->egl_handle) return STATUS_NOT_SUPPORTED;
    if (!(opengl_handle = dlopen( SONAME_LIBGLESV2, RTLD_NOW|RTLD_GLOBAL )))
    {
        ERR( "failed to load %s: %s\n", SONAME_LIBGLESV2, dlerror() );
        return STATUS_NOT_SUPPORTED;
    }
    funcs = opengl_funcs;

    android_driver_funcs.p_init_pixel_formats = (*driver_funcs)->p_init_pixel_formats;
    android_driver_funcs.p_describe_pixel_format = (*driver_funcs)->p_describe_pixel_format;
    android_driver_funcs.p_context_create = (*driver_funcs)->p_context_create;
    android_driver_funcs.p_context_destroy = (*driver_funcs)->p_context_destroy;
    android_driver_funcs.p_make_current = (*driver_funcs)->p_make_current;

    *driver_funcs = &android_driver_funcs;
    return STATUS_SUCCESS;
}
