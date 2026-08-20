/*
 * M1 spike: wasm32 CPU-bridge stubs for the ntdll unix side.
 *
 * These are the per-architecture functions normally provided by
 * signal_i386.c / signal_x86_64.c.  On wasm32 the "CPU" for PE code is an
 * emulator (to be wired in at these exact seams); for milestone 1 we only
 * need the unix side to boot as far as possible, so the execution entry
 * points abort loudly and the init paths succeed as no-ops.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winnt.h"
#include "winternl.h"
#include "unix_private.h"

static void bridge_unimplemented( const char *name )
{
    fprintf( stderr, "wine-wasm: CPU bridge entry '%s' reached (guest execution requested) — not yet implemented\n", name );
    abort();
}

/* ---- execution entry points: the emulator plugs in here later ---- */

void __wine_syscall_dispatcher(void) { bridge_unimplemented( "__wine_syscall_dispatcher" ); }
void __wine_syscall_dispatcher_return(void) { bridge_unimplemented( "__wine_syscall_dispatcher_return" ); }
void __wine_unix_call_dispatcher(void) { bridge_unimplemented( "__wine_unix_call_dispatcher" ); }

NTSTATUS signal_set_full_context( CONTEXT *context )
{
    bridge_unimplemented( "signal_set_full_context" );
    return STATUS_NOT_IMPLEMENTED;
}

/* User-mode callback (win32u -> user32 window procs etc.). Normally in
 * signal_i386.c; implemented in wasm_x86.c as a nested interpreter call. */
NTSTATUS wasm_x86_user_callback( ULONG id, const void *args, ULONG len, void **ret_ptr, ULONG *ret_len );
NTSTATUS KeUserModeCallback( ULONG id, const void *args, ULONG len, void **ret_ptr, ULONG *ret_len )
{
    return wasm_x86_user_callback( id, args, len, ret_ptr, ret_len );
}

NTSTATUS call_user_apc_dispatcher( CONTEXT *context_ptr, unsigned int flags, ULONG_PTR arg1, ULONG_PTR arg2,
                                   ULONG_PTR arg3, PNTAPCFUNC func, NTSTATUS status )
{
    bridge_unimplemented( "call_user_apc_dispatcher" );
    return STATUS_NOT_IMPLEMENTED;
}

extern void wasm_x86_setup_exception( EXCEPTION_RECORD *rec, CONTEXT *context );
NTSTATUS call_user_exception_dispatcher( struct thread_data *data, EXCEPTION_RECORD *rec, CONTEXT *context )
{
    /* EXCEPTION_WINE_STUB (0x80000100): a stubbed API was called; the two
     * params point to the module + function name strings.  Log them. */
    if (rec && rec->ExceptionCode == 0x80000100 && rec->NumberParameters >= 2)
        fprintf( stderr, "wine-wasm: STUB called: %s.%s\n",
                 (const char *)(uintptr_t)rec->ExceptionInformation[0],
                 (const char *)(uintptr_t)rec->ExceptionInformation[1] );
    /* Deliver the exception to the guest's SEH chain via the i386 interpreter. */
    wasm_x86_setup_exception( rec, context );
    return STATUS_SUCCESS;
}

void call_raise_user_exception_dispatcher( struct thread_data *data )
{
    bridge_unimplemented( "call_raise_user_exception_dispatcher" );
}

/* signal_start_thread + the syscall/unix-call dispatch seam now live in
 * wasm_x86.c (the standalone i386 interpreter). */

/* ---- init/config paths: succeed quietly so boot can continue ---- */

void signal_init_process( TEB *teb )
{
    fprintf( stderr, "wine-wasm: signal_init_process (noop)\n" );
}

NTSTATUS signal_alloc_thread( TEB *teb ) { return STATUS_SUCCESS; }
void signal_free_thread( TEB *teb ) { }
void signal_disable_syscall_dispatch(void) { }
void set_process_instrumentation_callback( void *callback ) { }

NTSTATUS get_thread_wow64_context( HANDLE handle, void *ctx, ULONG size ) { return STATUS_NOT_IMPLEMENTED; }
NTSTATUS set_thread_wow64_context( HANDLE handle, const void *ctx, ULONG size ) { return STATUS_NOT_IMPLEMENTED; }

/* ---- xattr shims: emscripten's libc has no xattr support ---- */

ssize_t getxattr( const char *path, const char *name, void *value, size_t size ) { return -1; }
ssize_t fgetxattr( int fd, const char *name, void *value, size_t size ) { return -1; }
int fsetxattr( int fd, const char *name, const void *value, size_t size, int flags ) { return -1; }
int fremovexattr( int fd, const char *name ) { return -1; }
int setxattr( const char *path, const char *name, const void *value, size_t size, int flags ) { return -1; }
int removexattr( const char *path, const char *name ) { return -1; }
ssize_t listxattr( const char *path, char *list, size_t size ) { return -1; }

/* ---- second link round: remaining per-arch symbols ---- */

NTSTATUS get_thread_ldt_entry( HANDLE handle, THREAD_DESCRIPTOR_INFORMATION *info, ULONG len )
{
    return STATUS_NOT_IMPLEMENTED;
}
void *get_native_context( CONTEXT *context ) { return context; }
void *get_wow_context( CONTEXT *context ) { return NULL; }

NTSTATUS WINAPI NtGetContextThread( HANDLE handle, CONTEXT *context ) { return STATUS_NOT_IMPLEMENTED; }
NTSTATUS WINAPI NtSetContextThread( HANDLE handle, const CONTEXT *context ) { return STATUS_NOT_IMPLEMENTED; }
NTSTATUS wasm_x86_callback_return( void *ret_ptr, unsigned int ret_len, int status );
NTSTATUS WINAPI NtCallbackReturn( PVOID ret_ptr, ULONG ret_len, NTSTATUS status )
{
    return wasm_x86_callback_return( ret_ptr, ret_len, status );
}

/* ---- identity: report the host's real uid/gid under NODERAWFS ---- */
#include <emscripten.h>
EM_JS(int, wasm_host_uid, (void), {
    try { return (typeof process !== 'undefined' && process.getuid) ? process.getuid() : 0; }
    catch(e) { return 0; }
});
EM_JS(int, wasm_host_gid, (void), {
    try { return (typeof process !== 'undefined' && process.getgid) ? process.getgid() : 0; }
    catch(e) { return 0; }
});
uid_t getuid(void)  { return wasm_host_uid(); }
uid_t geteuid(void) { return wasm_host_uid(); }
gid_t getgid(void)  { return wasm_host_gid(); }
gid_t getegid(void) { return wasm_host_gid(); }

/* getrusage: emscripten leaves __syscall_getrusage unimplemented, so it warns
 * on every call and returns zeroed time.  netduke32's frame timer polls process
 * CPU time (clock()/GetProcessTimes) in a tight loop; with time frozen at 0 it
 * busy-waits forever and floods stderr.  Provide a real, monotonically-advancing
 * ru_utime from the JS clock so the timer progresses and the flood stops. */
#include <sys/resource.h>
#include <sys/time.h>
#include <string.h>
EM_JS(double, wasm_now_ms, (void), {
    return (typeof performance !== 'undefined' && performance.now) ? performance.now() : Date.now();
});
int getrusage( int who, struct rusage *ru )
{
    if (ru)
    {
        double us = wasm_now_ms() * 1000.0;
        memset( ru, 0, sizeof(*ru) );
        ru->ru_utime.tv_sec  = (time_t)(us / 1000000.0);
        ru->ru_utime.tv_usec = (suseconds_t)(us - (double)ru->ru_utime.tv_sec * 1000000.0);
    }
    return 0;
}
