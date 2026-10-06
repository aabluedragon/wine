/*
 * Copyright 1995 Thomas Sandford (tdgsandf@prds-grn.demon.co.uk)
 * Copyright 2003 Dimitrie O. Paun
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

#include "ntstatus.h"
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "kernel_private.h"
#include "wine/list.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(powermgnt);

/******************************************************************************
 *           GetDevicePowerState   (KERNEL32.@)
 */
BOOL WINAPI GetDevicePowerState(HANDLE hDevice, BOOL* pfOn)
{
    WARN("(hDevice %p pfOn %p): stub\n", hDevice, pfOn);
    return TRUE; /* no information */
}

/***********************************************************************
 *           GetSystemPowerStatus      (KERNEL32.@)
 */
BOOL WINAPI GetSystemPowerStatus(LPSYSTEM_POWER_STATUS ps)
{
    SYSTEM_BATTERY_STATE bs;
    NTSTATUS status;

    TRACE("(%p)\n", ps);

    ps->ACLineStatus        = AC_LINE_UNKNOWN;
    ps->BatteryFlag         = BATTERY_FLAG_UNKNOWN;
    ps->BatteryLifePercent  = BATTERY_PERCENTAGE_UNKNOWN;
    ps->SystemStatusFlag    = 0;
    ps->BatteryLifeTime     = BATTERY_LIFE_UNKNOWN;
    ps->BatteryFullLifeTime = BATTERY_LIFE_UNKNOWN;

    status = NtPowerInformation(SystemBatteryState, NULL, 0, &bs, sizeof(bs));
    if (status == STATUS_NOT_IMPLEMENTED) return TRUE;
    if (FAILED(status)) return FALSE;

    ps->ACLineStatus = bs.AcOnLine;

    if (bs.BatteryPresent)
    {
        ps->BatteryLifePercent = bs.MaxCapacity ? 100 * bs.RemainingCapacity / bs.MaxCapacity : 100;
        ps->BatteryLifeTime = bs.EstimatedTime;
        if (!bs.Charging && (LONG)bs.Rate < 0)
            ps->BatteryFullLifeTime = 3600 * bs.MaxCapacity / -(LONG)bs.Rate;

        ps->BatteryFlag = 0;
        if (bs.Charging)
            ps->BatteryFlag |= BATTERY_FLAG_CHARGING;
        if (ps->BatteryLifePercent > 66)
            ps->BatteryFlag |= BATTERY_FLAG_HIGH;
        if (ps->BatteryLifePercent < 33)
            ps->BatteryFlag |= BATTERY_FLAG_LOW;
        if (ps->BatteryLifePercent < 5)
            ps->BatteryFlag |= BATTERY_FLAG_CRITICAL;
    }
    else
    {
        ps->BatteryFlag = BATTERY_FLAG_NO_BATTERY;
    }

    return TRUE;
}

/***********************************************************************
 *           IsSystemResumeAutomatic   (KERNEL32.@)
 */
BOOL WINAPI IsSystemResumeAutomatic(void)
{
    WARN("(): stub, harmless.\n");
    return FALSE;
}

/***********************************************************************
 *           RequestWakeupLatency      (KERNEL32.@)
 */
BOOL WINAPI RequestWakeupLatency(LATENCY_TIME latency)
{
    WARN("(): stub, harmless.\n");
    return TRUE;
}

/***********************************************************************
 *           RequestDeviceWakeup      (KERNEL32.@)
 */
BOOL WINAPI RequestDeviceWakeup(HANDLE device)
{
    FIXME("(%p): stub\n", device);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

/***********************************************************************
 *           SetSystemPowerState      (KERNEL32.@)
 */
BOOL WINAPI SetSystemPowerState(BOOL suspend_or_hibernate,
                                  BOOL force_flag)
{
    WARN("(): stub, harmless.\n");
    return TRUE;
}

/***********************************************************************
 * SetThreadExecutionState (KERNEL32.@)
 *
 * Informs the system that activity is taking place for
 * power management purposes.
 */
/* A power request and SetThreadExecutionState are two ways of asking for the
 * same thing, so they are kept together and the union of the two is what gets
 * asked of the system. */
struct power_request
{
    struct list entry;
    HANDLE handle;
    EXECUTION_STATE state;
};

static struct list power_requests = LIST_INIT( power_requests );
static EXECUTION_STATE continuous_state = ES_CONTINUOUS;

static CRITICAL_SECTION power_cs;
static CRITICAL_SECTION_DEBUG power_cs_debug =
{
    0, 0, &power_cs,
    { &power_cs_debug.ProcessLocksList, &power_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": power_cs") }
};
static CRITICAL_SECTION power_cs = { &power_cs_debug, -1, 0, 0, 0, 0 };

/* Returns the state that was in effect before this call. Must be called with
 * power_cs held. */
static EXECUTION_STATE update_execution_state( EXECUTION_STATE once )
{
    EXECUTION_STATE state = continuous_state, old;
    struct power_request *request;

    LIST_FOR_EACH_ENTRY( request, &power_requests, struct power_request, entry )
        state |= request->state;

    /* A request without ES_CONTINUOUS only resets the idle timers, so pass it
     * on by itself and leave the standing state alone. */
    NtSetThreadExecutionState( once ? once : state, &old );
    return old;
}

EXECUTION_STATE WINAPI SetThreadExecutionState(EXECUTION_STATE flags)
{
    EXECUTION_STATE old;

    TRACE("(0x%lx)\n", (DWORD)flags);

    EnterCriticalSection( &power_cs );
    if (flags & ES_CONTINUOUS) continuous_state = flags;
    old = update_execution_state( (flags & ES_CONTINUOUS) ? 0 : flags );
    LeaveCriticalSection( &power_cs );

    return old;
}

/* The flags a request of this type stands for. */
static EXECUTION_STATE execution_state_from_request_type( POWER_REQUEST_TYPE type )
{
    switch (type)
    {
    case PowerRequestDisplayRequired:   return ES_DISPLAY_REQUIRED;
    case PowerRequestSystemRequired:    return ES_SYSTEM_REQUIRED;
    case PowerRequestAwayModeRequired:  return ES_AWAYMODE_REQUIRED;
    /* Keeps the process running rather than the machine awake, but the only
     * thing that would stop it is the system going to sleep. */
    case PowerRequestExecutionRequired: return ES_SYSTEM_REQUIRED;
    default:                            return 0;
    }
}

/* Must be called with power_cs held. */
static struct power_request *find_power_request( HANDLE handle )
{
    struct power_request *request;

    LIST_FOR_EACH_ENTRY( request, &power_requests, struct power_request, entry )
        if (request->handle == handle) return request;

    return NULL;
}

/***********************************************************************
 *           PowerCreateRequest      (KERNEL32.@)
 */
HANDLE WINAPI PowerCreateRequest(REASON_CONTEXT *context)
{
    struct power_request *request;
    HANDLE handle;

    TRACE("(%p)\n", context);

    if (!context)
    {
        SetLastError( ERROR_INVALID_PARAMETER );
        return INVALID_HANDLE_VALUE;
    }

    /* The reason is only ever shown by powercfg, which has nothing to read it
     * from here. The handle has to be one CloseHandle accepts. */
    if (!(handle = CreateEventW( NULL, TRUE, FALSE, NULL ))) return INVALID_HANDLE_VALUE;

    if (!(request = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*request) )))
    {
        CloseHandle( handle );
        SetLastError( ERROR_OUTOFMEMORY );
        return INVALID_HANDLE_VALUE;
    }
    request->handle = handle;

    EnterCriticalSection( &power_cs );
    list_add_tail( &power_requests, &request->entry );
    LeaveCriticalSection( &power_cs );

    return handle;
}

/***********************************************************************
 *           PowerSetRequest      (KERNEL32.@)
 */
BOOL WINAPI PowerSetRequest(HANDLE request, POWER_REQUEST_TYPE type)
{
    EXECUTION_STATE state = execution_state_from_request_type( type );
    struct power_request *entry;

    TRACE("(%p, %u)\n", request, type);

    if (!state)
    {
        SetLastError( ERROR_INVALID_PARAMETER );
        return FALSE;
    }

    EnterCriticalSection( &power_cs );
    if ((entry = find_power_request( request )))
    {
        entry->state |= state;
        update_execution_state( 0 );
    }
    LeaveCriticalSection( &power_cs );

    if (!entry)
    {
        SetLastError( ERROR_INVALID_HANDLE );
        return FALSE;
    }
    return TRUE;
}

/***********************************************************************
 *           PowerClearRequest      (KERNEL32.@)
 */
BOOL WINAPI PowerClearRequest(HANDLE request, POWER_REQUEST_TYPE type)
{
    EXECUTION_STATE state = execution_state_from_request_type( type );
    struct power_request *entry;

    TRACE("(%p, %u)\n", request, type);

    if (!state)
    {
        SetLastError( ERROR_INVALID_PARAMETER );
        return FALSE;
    }

    EnterCriticalSection( &power_cs );
    if ((entry = find_power_request( request )))
    {
        entry->state &= ~state;
        update_execution_state( 0 );
    }
    LeaveCriticalSection( &power_cs );

    if (!entry)
    {
        SetLastError( ERROR_INVALID_HANDLE );
        return FALSE;
    }
    return TRUE;
}
