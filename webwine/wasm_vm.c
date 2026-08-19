/*
 * wasm32 virtual-memory shim for the Wine unix side.
 *
 * The wasm32 linear memory IS the guest i386 address space (identity
 * mapping): guest pointers and native pointers are the same numbers.
 * Layout:
 *   [0x00000000, GLOBAL_BASE)  guest space, owned by Wine's virtual.c
 *                              through this mmap implementation
 *   [GLOBAL_BASE=0x70000000, ~2GB)  native Wine (wasm data, stack, heap)
 *   0x7ffe0000                 KUSER_SHARED_DATA (fixed map honored;
 *                              native sbrk should never reach it)
 *
 * Linear memory is fully pre-grown (INITIAL_MEMORY = 2GB), so "mapping"
 * is pure bookkeeping: MAP_FIXED zeroes and returns the address,
 * anonymous allocation bumps through the guest region, and file maps
 * pread the contents into place.  There is no page protection in wasm;
 * PROT_* is accepted and ignored (SMC detection etc. is the emulator's
 * concern, as in any wasm x86 emulator).
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#define GUEST_SPACE_END   0x70000000u   /* == GLOBAL_BASE of the native module */
#define GUEST_BUMP_START  0x20000000u   /* non-fixed anonymous allocations */
#define SHARED_DATA_ADDR  0x7ffe0000u
#define WASM_PAGE_MASK    0xffffu

static uint32_t bump_next = GUEST_BUMP_START;
static int trace_vm = -1;

static int vm_trace(void)
{
    if (trace_vm < 0)
    {
        const char *e = getenv( "WINEWASMVMTRACE" );
        trace_vm = e && *e && *e != '0';
    }
    return trace_vm;
}

void *mmap( void *addr, size_t len, int prot, int flags, int fd, off_t offset )
{
    uint32_t a = (uint32_t)(uintptr_t)addr;
    uint32_t size = (uint32_t)((len + 0xfff) & ~0xfffu);

    if (flags & MAP_FIXED)
    {
        if (a + size < a) { errno = EINVAL; return MAP_FAILED; }
        if (a + size > GUEST_SPACE_END &&
            !(a >= SHARED_DATA_ADDR && a + size <= SHARED_DATA_ADDR + 0x10000))
        {
            if (vm_trace())
                fprintf( stderr, "wasm_vm: refusing MAP_FIXED %08x-%08x inside native region\n", a, a + size );
            errno = ENOMEM;
            return MAP_FAILED;
        }
    }
    else if (a && a + size > a &&
             (a + size <= GUEST_SPACE_END ||
              (a >= SHARED_DATA_ADDR && a + size <= SHARED_DATA_ADDR + 0x10000)))
    {
        /* Non-fixed with an address hint: Wine's view manager picks free
         * addresses itself and passes them as hints, checking that the result
         * equals the hint.  It is the sole owner of the guest region, so its
         * hints are authoritative — honor them exactly. */
    }
    else if (prot == PROT_NONE)
    {
        /* Address-space reservation/probe: hand out addresses up to the top
         * of the i386 user space WITHOUT touching memory, so Wine discovers a
         * full 2GB address space (the shared-user-data page lives at
         * 0x7ffe0000).  Real (readable/writable) mappings into the native
         * region are still refused above. */
        a = bump_next;
        if (a + size > 0x7fff0000u || a + size < a) { errno = ENOMEM; return MAP_FAILED; }
        bump_next = a + size;
        if (vm_trace())
            fprintf( stderr, "wasm_vm: reserve %08x-%08x\n", a, a + size );
        return (void *)(uintptr_t)a;
    }
    else
    {
        /* ignore the hint; allocate from the guest bump region */
        a = bump_next;
        if (a + size > GUEST_SPACE_END) { errno = ENOMEM; return MAP_FAILED; }
        bump_next = a + size;
    }

    if (fd == -1)
    {
        if (a + size <= GUEST_SPACE_END || (a >= SHARED_DATA_ADDR && a + size <= SHARED_DATA_ADDR + 0x10000))
            memset( (void *)(uintptr_t)a, 0, size );
    }
    else
    {
        ssize_t r = pread( fd, (void *)(uintptr_t)a, len, offset );
        if (r < 0) { errno = EACCES; return MAP_FAILED; }
        if ((size_t)r < size) memset( (char *)(uintptr_t)a + r, 0, size - r );
    }
    if (vm_trace())
        fprintf( stderr, "wasm_vm: mmap %08x-%08x prot=%x flags=%x fd=%d off=%llx\n",
                 a, a + size, prot, flags, fd, (long long)offset );
    return (void *)(uintptr_t)a;
}

int munmap( void *addr, size_t len )
{
    uint32_t a = (uint32_t)(uintptr_t)addr;
    uint32_t size = (uint32_t)((len + 0xfff) & ~0xfffu);

    if (vm_trace())
        fprintf( stderr, "wasm_vm: munmap %08x len=%zx\n", a, len );
    /* LIFO reclaim: address-space probes mmap huge blocks and munmap them
     * immediately; give the space back when the freed range ends at the
     * current bump pointer so probes don't exhaust the region. */
    if (a + size == bump_next && a >= GUEST_BUMP_START) bump_next = a;
    /* the memory itself stays; zero it so a later map sees fresh pages */
    if (a + len <= GUEST_SPACE_END) memset( addr, 0, len );
    return 0;
}

int mprotect( void *addr, size_t len, int prot ) { return 0; }
int mlock( const void *addr, size_t len ) { return 0; }
int munlock( const void *addr, size_t len ) { return 0; }
int madvise( void *addr, size_t len, int advice ) { return 0; }

/* Wine probes whether a range is FREE by calling msync/mincore and treating
 * ENOMEM ("not mapped") as free.  We do not track mappings, and the guest
 * region is always available, so report every range as unmapped — a fixed-map
 * conflict check then always concludes the space is free, which matches the
 * semantics of this always-backed linear memory. */
int msync( void *addr, size_t len, int flags ) { errno = ENOMEM; return -1; }
int mincore( void *addr, size_t len, unsigned char *vec ) { errno = ENOMEM; return -1; }

/* keep signature drift visible but non-fatal across emscripten versions */
#ifdef __cplusplus
#error plain C only
#endif
