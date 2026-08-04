/*
 * Copyright 2010 Louis Lenders
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

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "wine/debug.h"

#include "restartmanager.h"

WINE_DEFAULT_DEBUG_CHANNEL(rstrtmgr);

/***********************************************************************
 * RmGetList (rstrtmgr.@)
 *
 * Retrieve the list of all applications and services using resources registered with the Restart Manager session
 */
DWORD WINAPI RmGetList(DWORD dwSessionHandle, UINT *pnProcInfoNeeded, UINT *pnProcInfo,
                                      RM_PROCESS_INFO *rgAffectedApps[], LPDWORD lpdwRebootReasons)
{
    FIXME("%ld, %p, %p, %p, %p stub!\n", dwSessionHandle, pnProcInfoNeeded, pnProcInfo, rgAffectedApps, lpdwRebootReasons);
    if (pnProcInfoNeeded)
        *pnProcInfoNeeded = 0;
    if (pnProcInfo)
        *pnProcInfo = 0;
    return ERROR_SUCCESS;
}

/***********************************************************************
 * RmRegisterResources (rstrtmgr.@)
 *
 * Register resources for a Restart Manager session
 */
DWORD WINAPI RmRegisterResources(DWORD dwSessionHandle, UINT nFiles, LPCWSTR rgsFilenames[],
                                                     UINT nApplications, RM_UNIQUE_PROCESS *rgApplications,
                                                     UINT nServices, LPCWSTR rgsServiceNames[])
{
    FIXME("%ld, %d, %p, %d, %p, %d, %p stub!\n", dwSessionHandle, nFiles, rgsFilenames,
              nApplications, rgApplications, nServices, rgsServiceNames);
    return ERROR_SUCCESS;
}

/***********************************************************************
 * RmStartSession (rstrtmgr.@)
 *
 * Start a new Restart Manager session
 */
DWORD WINAPI RmStartSession(DWORD *sessionhandle, DWORD flags, WCHAR sessionkey[])
{
    static const WCHAR hex[] = L"0123456789abcdef";
    static LONG session_count;
    ULONGLONG key[2];
    const BYTE *bytes;
    UINT i;

    FIXME("%p, %ld, %p stub!\n", sessionhandle, flags, sessionkey);

    if (!sessionhandle || !sessionkey) return ERROR_INVALID_PARAMETER;

    /* The caller passes an uninitialized CCH_RM_SESSION_KEY + 1 buffer that has to be
     * filled with a null terminated key. Applications use it as a string right away
     * (Inno Setup based installers do), so leaving it untouched makes them read past
     * the buffer and corrupt their stack. */
    key[0] = (ULONGLONG)GetCurrentProcessId() << 32 | GetTickCount();
    key[1] = (ULONGLONG)InterlockedIncrement( &session_count ) << 32 | GetCurrentThreadId();
    bytes = (const BYTE *)key;
    for (i = 0; i < sizeof(key); i++)
    {
        sessionkey[2 * i] = hex[bytes[i] >> 4];
        sessionkey[2 * i + 1] = hex[bytes[i] & 0xf];
    }
    sessionkey[CCH_RM_SESSION_KEY] = 0;

    *sessionhandle = 0xdeadbeef;
    return ERROR_SUCCESS;
}

/***********************************************************************
 * RmRestart (rstrtmgr.@)
 */
DWORD WINAPI RmRestart(DWORD handle, DWORD flags, RM_WRITE_STATUS_CALLBACK status)
{
    FIXME("%lu, 0x%08lx, %p stub!\n", handle, flags, status);
    return ERROR_SUCCESS;
}

/***********************************************************************
 * RmEndSession (rstrtmgr.@)
 */
DWORD WINAPI RmEndSession(DWORD handle)
{
    FIXME("%lu stub!\n", handle);
    return ERROR_SUCCESS;
}

/***********************************************************************
 * RmShutdown (rstrtmgr.@)
 */
DWORD WINAPI RmShutdown(DWORD handle, ULONG flags, RM_WRITE_STATUS_CALLBACK status)
{
    FIXME("%lu, 0x%08lx, %p stub!\n", handle, flags, status);
    return ERROR_SUCCESS;
}

/***********************************************************************
 * RmAddFilter (rstrtmgr.@)
 */
DWORD WINAPI RmAddFilter(DWORD handle, LPCWSTR moduleName, RM_UNIQUE_PROCESS *process,
                         LPCWSTR serviceShortName, RM_FILTER_ACTION filter)
{
    FIXME("%lu, %s %p %s 0x%08x stub!\n", handle, debugstr_w(moduleName), process,
        debugstr_w(serviceShortName), filter);
    return ERROR_SUCCESS;
}

/***********************************************************************
 * RmRemoveFilter (rstrtmgr.@)
 */
DWORD WINAPI RmRemoveFilter(DWORD handle, LPCWSTR moduleName, RM_UNIQUE_PROCESS *process,
                            LPCWSTR serviceShortName)
{
    FIXME("%lu, %s %p %s stub!\n", handle, debugstr_w(moduleName), process,
        debugstr_w(serviceShortName));
    return ERROR_SUCCESS;
}
