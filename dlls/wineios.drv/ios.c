/*
 * iOS graphics driver
 *
 * Copyright 2026 Alon Amir
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

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pthread.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "ntgdi.h"
#include "ntuser.h"
#include "wine/gdi_driver.h"
#include "wine/server.h"

#include "unixlib.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(ios);

/* iOS hands an application one screen and does not let it choose the size, so
 * there is one source with one mode. A UIKit host tells us the size it has;
 * until then, take a plausible one. */
static unsigned int screen_width = 1179;
static unsigned int screen_height = 2556;
static unsigned int screen_bpp = 32;

/* The window contents are handed to a UIKit host through a framebuffer both
 * sides map: iOS gives a process no way to put pixels on the screen itself,
 * and the host is a separate process. The header lets the host pick up the
 * size and notice new frames. */
struct framebuffer
{
    UINT magic;
    UINT width;
    UINT height;
    UINT generation;
    /* rect of the main (game) window's client area within the desktop, so the
     * host can crop/zoom to just the game instead of showing the whole desktop */
    UINT win_x;
    UINT win_y;
    UINT win_w;
    UINT win_h;
};

#define IOS_FRAMEBUFFER_MAGIC 0x534f4957  /* WIOS */

static struct framebuffer *framebuffer;
static pthread_mutex_t framebuffer_mutex = PTHREAD_MUTEX_INITIALIZER;

static UINT *framebuffer_pixels(void)
{
    return (UINT *)(framebuffer + 1);
}

static BOOL map_framebuffer(void)
{
    const char *path = getenv( "WINE_IOS_FRAMEBUFFER" );
    size_t size;
    void *ptr;
    int fd;

    if (framebuffer) return TRUE;
    if (!path) path = "/tmp/wine-ios-framebuffer";

    size = sizeof(*framebuffer) + (size_t)screen_width * screen_height * 4;
    if ((fd = open( path, O_RDWR | O_CREAT, 0600 )) == -1)
    {
        WARN( "cannot open %s\n", path );
        return FALSE;
    }
    if (ftruncate( fd, size ))
    {
        close( fd );
        return FALSE;
    }
    ptr = mmap( NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 );
    close( fd );
    if (ptr == MAP_FAILED)
    {
        WARN( "cannot map %s\n", path );
        return FALSE;
    }

    framebuffer = ptr;
    framebuffer->magic = IOS_FRAMEBUFFER_MAGIC;
    framebuffer->width = screen_width;
    framebuffer->height = screen_height;
    framebuffer->win_x = 0;
    framebuffer->win_y = 0;
    framebuffer->win_w = screen_width;
    framebuffer->win_h = screen_height;
    TRACE( "presenting through %s, %ux%u\n", path, screen_width, screen_height );
    return TRUE;
}

/* The host application sees the touches and writes them into a fifo. The fifo
 * is what makes them arrive: the server watches the descriptor and only then
 * does win32u ask the driver to process events. */
struct input_event
{
    UINT type;      /* 1 move, 2 button down, 3 button up, 4 key down, 5 key up */
    INT  x;         /* screen pixels, or the virtual key for a key event */
    INT  y;         /* screen pixels, or the scan code for a key event */
};

static int input_fd = -1;
static DWORD desktop_tid;

static void init_input(void)
{
    const char *path = getenv( "WINE_IOS_INPUT" );
    HANDLE handle;

    if (input_fd != -1) return;
    if (!path) path = "/tmp/wine-ios-input";

    mkfifo( path, 0600 );
    if ((input_fd = open( path, O_RDWR | O_NONBLOCK )) == -1)
    {
        WARN( "cannot open %s\n", path );
        return;
    }

    if (wine_server_fd_to_handle( input_fd, GENERIC_READ | SYNCHRONIZE, 0, &handle ))
    {
        ERR( "cannot make a handle for the input fifo\n" );
        return;
    }
    SERVER_START_REQ( set_queue_fd )
    {
        req->handle = wine_server_obj_handle( handle );
        wine_server_call( req );
    }
    SERVER_END_REQ;
    NtClose( handle );
    desktop_tid = GetCurrentThreadId();
    TRACE( "reading input from %s\n", path );
}

/**********************************************************************
 *           IOS_ProcessEvents
 */
static BOOL IOS_ProcessEvents( DWORD mask )
{
    struct input_event event;
    BOOL ret = FALSE;

    if (input_fd == -1) return FALSE;

    while (read( input_fd, &event, sizeof(event) ) == sizeof(event))
    {
        INPUT input = {0};

        if (event.type == 4 || event.type == 5)
        {
            input.type = INPUT_KEYBOARD;
            input.ki.wVk = event.x;
            input.ki.wScan = event.y;
            input.ki.dwFlags = event.type == 5 ? KEYEVENTF_KEYUP : 0;
            TRACE( "key %s vk %#x scan %#x\n", event.type == 5 ? "up" : "down",
                   (unsigned int)event.x, (unsigned int)event.y );
        }
        else
        {
            input.type = INPUT_MOUSE;
            input.mi.dx = event.x;
            input.mi.dy = event.y;
            switch (event.type)
            {
            case 2: input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_LEFTDOWN; break;
            case 3: input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_LEFTUP; break;
            default: input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE; break;
            }
            TRACE( "pointer %u at %d,%d\n", event.type, event.x, event.y );
        }

        NtUserSendHardwareInput( 0, 0, &input, 0 );
        ret = TRUE;
    }
    return ret;
}

struct ios_window_surface
{
    struct window_surface header;
};

static void ios_surface_set_clip( struct window_surface *surface, const RECT *rects, UINT count )
{
}

static BOOL ios_surface_flush( struct window_surface *surface, const RECT *rect, const RECT *dirty,
                               const BITMAPINFO *color_info, const void *color_bits, BOOL shape_changed,
                               const BITMAPINFO *shape_info, const void *shape_bits )
{
    const UINT *src = color_bits;
    UINT src_width = color_info->bmiHeader.biWidth;
    int x, y;

    pthread_mutex_lock( &framebuffer_mutex );
    if (map_framebuffer())
    {
        UINT *dst = framebuffer_pixels();

        for (y = dirty->top; y < dirty->bottom; y++)
        {
            int screen_y = rect->top + y;

            if (screen_y < 0 || screen_y >= (int)framebuffer->height) continue;
            for (x = dirty->left; x < dirty->right; x++)
            {
                int screen_x = rect->left + x;

                if (screen_x < 0 || screen_x >= (int)framebuffer->width) continue;
                dst[screen_y * framebuffer->width + screen_x] = src[y * src_width + x] | 0xff000000;
            }
        }
        /* Report the main (game) window's client rect so the host can zoom to it.
         * Filter to reasonably large surfaces so small startup dialogs don't
         * override the game window. */
        if (color_info->bmiHeader.biWidth >= 640 && abs( color_info->bmiHeader.biHeight ) >= 400 &&
            rect->left >= 0 && rect->top >= 0)
        {
            framebuffer->win_x = rect->left;
            framebuffer->win_y = rect->top;
            framebuffer->win_w = color_info->bmiHeader.biWidth;
            framebuffer->win_h = abs( color_info->bmiHeader.biHeight );
        }
        framebuffer->generation++;
    }
    pthread_mutex_unlock( &framebuffer_mutex );

    return TRUE;
}

static void ios_surface_destroy( struct window_surface *surface )
{
}

static const struct window_surface_funcs ios_surface_funcs =
{
    ios_surface_set_clip,
    ios_surface_flush,
    ios_surface_destroy,
};

/**********************************************************************
 *           IOS_CreateWindowSurface
 */
static BOOL IOS_CreateWindowSurface( HWND hwnd, BOOL layered, const RECT *surface_rect, struct window_surface **surface )
{
    char buffer[FIELD_OFFSET( BITMAPINFO, bmiColors[256] )];
    BITMAPINFO *info = (BITMAPINFO *)buffer;
    int width = surface_rect->right - surface_rect->left;
    int height = surface_rect->bottom - surface_rect->top;

    TRACE( "hwnd %p, %s\n", hwnd, wine_dbgstr_rect( surface_rect ) );

    if (*surface && (*surface)->funcs == &ios_surface_funcs) return TRUE;
    if (*surface) window_surface_release( *surface );

    memset( info, 0, sizeof(*info) );
    info->bmiHeader.biSize        = sizeof(info->bmiHeader);
    info->bmiHeader.biWidth       = width;
    info->bmiHeader.biHeight      = -height;  /* top-down */
    info->bmiHeader.biPlanes      = 1;
    info->bmiHeader.biBitCount    = 32;
    info->bmiHeader.biSizeImage   = width * height * 4;
    info->bmiHeader.biCompression = BI_RGB;

    *surface = window_surface_create( sizeof(struct ios_window_surface), &ios_surface_funcs,
                                      hwnd, surface_rect, info, 0 );
    return TRUE;
}

/**********************************************************************
 *           IOS_UpdateDisplayDevices
 */
static UINT IOS_UpdateDisplayDevices( const struct gdi_device_manager *device_manager, void *param )
{
    static const DWORD source_flags = DISPLAY_DEVICE_ATTACHED_TO_DESKTOP |
                                      DISPLAY_DEVICE_PRIMARY_DEVICE | DISPLAY_DEVICE_VGA_COMPATIBLE;
    struct pci_id pci_id = {0};
    RECT rect = { 0, 0, screen_width, screen_height };
    struct gdi_monitor monitor =
    {
        .rc_monitor = rect,
        .rc_work = rect,
    };
    const DEVMODEW mode =
    {
        .dmSize = sizeof(mode),
        .dmFields = DM_DISPLAYORIENTATION | DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL |
                    DM_DISPLAYFLAGS | DM_DISPLAYFREQUENCY,
        .dmBitsPerPel = screen_bpp,
        .dmPelsWidth = screen_width,
        .dmPelsHeight = screen_height,
        .dmDisplayFrequency = 60,
    };
    UINT dpi = NtUserGetSystemDpiForProcess( NULL );
    DEVMODEW current = mode;

    TRACE( "%ux%u\n", screen_width, screen_height );

    device_manager->add_gpu( NULL, &pci_id, NULL, param );
    device_manager->add_source( "Default", source_flags, dpi, param );
    device_manager->add_monitor( &monitor, param );

    current.dmFields |= DM_POSITION;
    device_manager->add_modes( &current, 1, &mode, param );

    return STATUS_SUCCESS;
}

/**********************************************************************
 *           IOS_CreateWindow
 *
 * Windows are drawn into the offscreen surface win32u creates for a driver
 * that offers none, which is what makes the rest of the desktop work here.
 */
static BOOL IOS_CreateWindow( HWND hwnd )
{
    TRACE( "hwnd %p\n", hwnd );
    /* the queue the fifo is registered with is the one of the thread that
     * pumps messages, which is the thread creating the windows */
    init_input();
    return TRUE;
}

/**********************************************************************
 *           IOS_CreateDesktop
 */
static BOOL IOS_CreateDesktop( const WCHAR *name, UINT width, UINT height )
{
    TRACE( "%s %ux%u\n", debugstr_w(name), width, height );
    init_input();
    return TRUE;
}

static const struct user_driver_funcs ios_drv_funcs =
{
    .pUpdateDisplayDevices = IOS_UpdateDisplayDevices,
    .pCreateDesktop = IOS_CreateDesktop,
    .pCreateWindow = IOS_CreateWindow,
    .pCreateWindowSurface = IOS_CreateWindowSurface,
    .pProcessEvents = IOS_ProcessEvents,
};

NTSTATUS ios_init( void *args )
{
    const char *w = getenv( "WINE_IOS_SCREEN_WIDTH" );
    const char *h = getenv( "WINE_IOS_SCREEN_HEIGHT" );

    /* The desktop (and thus the shared framebuffer) defaults to the device's
     * native portrait resolution. A landscape game like Duke3D wants to run
     * fullscreen filling a landscape screen, so allow the host app to override
     * the desktop size and drive a landscape, phone-aspect resolution. */
    if (w && h)
    {
        unsigned int nw = atoi( w ), nh = atoi( h );
        if (nw >= 320 && nh >= 200) { screen_width = nw; screen_height = nh; }
    }

    TRACE( "registering the iOS graphics driver, desktop %ux%u\n", screen_width, screen_height );
    __wine_set_user_driver( &ios_drv_funcs, WINE_GDI_DRIVER_VERSION );
    return STATUS_SUCCESS;
}
