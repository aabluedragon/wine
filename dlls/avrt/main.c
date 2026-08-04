/* Avrt dll implementation
 *
 * Copyright (C) 2009 Maarten Lankhorst
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>
#include <stdlib.h>

#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "winuser.h"
#include "wine/debug.h"
#include "avrt.h"

WINE_DEFAULT_DEBUG_CHANNEL(avrt);

static inline WCHAR *strdupAW(const char *src)
{
    int len;
    WCHAR *dst;
    if (!src) return NULL;
    len = MultiByteToWideChar(CP_ACP, 0, src, -1, NULL, 0);
    if ((dst = malloc(len * sizeof(*dst)))) MultiByteToWideChar(CP_ACP, 0, src, -1, dst, len);
    return dst;
}

HANDLE WINAPI AvSetMmThreadCharacteristicsA(const char *name, DWORD *index)
{
    WCHAR *nameW = NULL;
    HANDLE ret;

    if (name && !(nameW = strdupAW(name)))
    {
        SetLastError(ERROR_OUTOFMEMORY);
        return NULL;
    }

    ret = AvSetMmThreadCharacteristicsW(nameW, index);

    free(nameW);
    return ret;
}

/* The tasks the Multimedia Class Scheduler Service knows about by default.
 * Windows looks them up under
 * SYSTEM\\CurrentControlSet\\Control\\MultimediaSettings\\SystemProfile\\Tasks. */
static const WCHAR *const mmcss_tasks[] =
{
    L"Audio", L"Capture", L"DisplayPostProcessing", L"Distribution",
    L"Games", L"Playback", L"Pro Audio", L"Window Manager",
};

struct mm_thread
{
    DWORD magic;
    int old_priority;
};

#define MM_THREAD_MAGIC 0x41565254 /* "AVRT" */

static struct mm_thread *mm_thread_from_handle(HANDLE handle)
{
    struct mm_thread *thread = handle;

    if (!thread || thread->magic != MM_THREAD_MAGIC)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return NULL;
    }
    return thread;
}

HANDLE WINAPI AvSetMmThreadCharacteristicsW(const WCHAR *name, DWORD *index)
{
    struct mm_thread *thread;
    unsigned int i;

    TRACE("(%s,%p)\n", debugstr_w(name), index);

    if (!name)
    {
        SetLastError(ERROR_INVALID_TASK_NAME);
        return NULL;
    }

    for (i = 0; i < ARRAY_SIZE(mmcss_tasks); i++)
        if (!wcsicmp(name, mmcss_tasks[i])) break;

    if (i == ARRAY_SIZE(mmcss_tasks))
    {
        WARN("Unknown task %s.\n", debugstr_w(name));
        SetLastError(ERROR_INVALID_TASK_NAME);
        return NULL;
    }

    if (!index)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return NULL;
    }

    if (!(thread = malloc(sizeof(*thread))))
    {
        SetLastError(ERROR_OUTOFMEMORY);
        return NULL;
    }
    thread->magic = MM_THREAD_MAGIC;
    thread->old_priority = GetThreadPriority(GetCurrentThread());

    /* Registering alone doesn't change the priority on Windows either; it only
     * makes the thread eligible for the guaranteed share of CPU time that we
     * have no way of reserving. AvSetMmThreadPriority() is what asks for a
     * priority, and that we can do. */
    *index = i + 1;
    return thread;
}

BOOL WINAPI AvQuerySystemResponsiveness(HANDLE AvrtHandle, ULONG *value)
{
    FIXME("(%p, %p): stub\n", AvrtHandle, value);
    return FALSE;
}

BOOL WINAPI AvRevertMmThreadCharacteristics(HANDLE AvrtHandle)
{
    struct mm_thread *thread;

    TRACE("(%p)\n", AvrtHandle);

    if (!(thread = mm_thread_from_handle(AvrtHandle))) return FALSE;

    SetThreadPriority(GetCurrentThread(), thread->old_priority);
    thread->magic = 0;
    free(thread);
    return TRUE;
}

BOOL WINAPI AvSetMmThreadPriority(HANDLE AvrtHandle, AVRT_PRIORITY prio)
{
    int priority;

    TRACE("(%p)->(%d)\n", AvrtHandle, prio);

    if (!mm_thread_from_handle(AvrtHandle)) return FALSE;

    switch (prio)
    {
    case AVRT_PRIORITY_VERYLOW:  priority = THREAD_PRIORITY_LOWEST; break;
    case AVRT_PRIORITY_LOW:      priority = THREAD_PRIORITY_BELOW_NORMAL; break;
    case AVRT_PRIORITY_NORMAL:   priority = THREAD_PRIORITY_NORMAL; break;
    case AVRT_PRIORITY_HIGH:     priority = THREAD_PRIORITY_ABOVE_NORMAL; break;
    case AVRT_PRIORITY_CRITICAL: priority = THREAD_PRIORITY_HIGHEST; break;
    default:
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    return SetThreadPriority(GetCurrentThread(), priority);
}

HANDLE WINAPI AvSetMmMaxThreadCharacteristicsA(const char *task1, const char *task2, DWORD *index)
{
    WCHAR *task1W = NULL, *task2W = NULL;
    HANDLE ret;

    if (task1 && !(task1W = strdupAW(task1)))
    {
        SetLastError(ERROR_OUTOFMEMORY);
        return NULL;
    }

    if (task2 && !(task2W = strdupAW(task2)))
    {
        SetLastError(ERROR_OUTOFMEMORY);
        return NULL;
    }

    ret = AvSetMmMaxThreadCharacteristicsW(task1W, task2W, index);

    free(task2W);
    free(task1W);
    return ret;
}

HANDLE WINAPI AvSetMmMaxThreadCharacteristicsW(const WCHAR *task1, const WCHAR *task2, DWORD *index)
{
    TRACE("(%s,%s,%p)\n", debugstr_w(task1), debugstr_w(task2), index);

    if (!task1 || task2)
    {
        SetLastError(ERROR_INVALID_TASK_NAME);
        return NULL;
    }

    return AvSetMmThreadCharacteristicsW(task1, index);
}
