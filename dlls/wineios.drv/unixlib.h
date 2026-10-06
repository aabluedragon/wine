/*
 * Unix call interface for wineios.drv
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

#include "../mmdevapi/unixlib.h"

/* This module is the audio driver as well as the graphics driver, and a module
 * has only one unix call table. mmdevapi owns the entries it defines, and the
 * driver's own initialisation follows them - it cannot be left to
 * __wine_unix_lib_init, which ntdll only looks for in a module that has no
 * table of its own. */
enum ios_funcs
{
    unix_init = funcs_count,
    unix_funcs_count
};

#ifdef WINE_UNIX_LIB
extern NTSTATUS ios_init( void *args );
#endif
