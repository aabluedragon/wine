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

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "ntgdi.h"
#include "wine/gdi_driver.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(ios);

/* iOS hands an application one screen and does not let it choose the size, so
 * there is one source with one mode. A UIKit host tells us the size it has;
 * until then, take a plausible one. */
static unsigned int screen_width = 1179;
static unsigned int screen_height = 2556;
static unsigned int screen_bpp = 32;

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
    return TRUE;
}

/**********************************************************************
 *           IOS_CreateDesktop
 */
static BOOL IOS_CreateDesktop( const WCHAR *name, UINT width, UINT height )
{
    TRACE( "%s %ux%u\n", debugstr_w(name), width, height );
    return TRUE;
}

static const struct user_driver_funcs ios_drv_funcs =
{
    .pUpdateDisplayDevices = IOS_UpdateDisplayDevices,
    .pCreateDesktop = IOS_CreateDesktop,
    .pCreateWindow = IOS_CreateWindow,
};

NTSTATUS __wine_unix_lib_init(void)
{
    __wine_set_user_driver( &ios_drv_funcs, WINE_GDI_DRIVER_VERSION );
    return STATUS_SUCCESS;
}
