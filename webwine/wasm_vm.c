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
#include <sys/stat.h>
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

/* Shared (MAP_SHARED) file mappings must be coherent between the in-process
 * wineserver (writer) and the PE-side libs (readers) — e.g. the win32u session
 * shared memory. With no MMU we can't alias two addresses to one page, but both
 * sides map with base=NULL, so we back all MAP_SHARED views of the same file by
 * ONE whole-file buffer (keyed by inode) and hand out base+offset into it. */
/* canonical whole-file buffers, keyed by inode (the server's writable view) */
struct shmap { uint64_t ino; uint32_t base; uint32_t fsize; };
static struct shmap g_shmaps[64];
static int g_n_shmaps;
/* read-only client views (win32u): mirrors re-synced from the canonical buffer
 * after every server interaction (wasm_vm_sync_shared) so the server's writes
 * become visible without an MMU to alias the two addresses. */
struct shmirror { uint32_t canon; uint32_t addr; uint32_t size; };
static struct shmirror g_mirrors[64];
static int g_n_mirrors;

static struct shmap *find_shmap( uint64_t ino )
{
    int i;
    for (i = 0; i < g_n_shmaps; i++) if (g_shmaps[i].ino == ino) return &g_shmaps[i];
    return 0;
}

/* re-copy every registered mirror from its canonical buffer (called from the
 * transport after driving the in-process server). */
void wasm_vm_sync_shared( void )
{
    int i;
    for (i = 0; i < g_n_mirrors; i++)
        memcpy( (void *)(uintptr_t)g_mirrors[i].addr, (void *)(uintptr_t)g_mirrors[i].canon, g_mirrors[i].size );
}

void *mmap( void *addr, size_t len, int prot, int flags, int fd, off_t offset )
{
    uint32_t a = (uint32_t)(uintptr_t)addr;
    uint32_t size = (uint32_t)((len + 0xfff) & ~0xfffu);

    if (fd != -1)
    {
        struct stat st;
        if (fstat( fd, &st ) == 0 && st.st_ino)
        {
            struct shmap *sm = find_shmap( st.st_ino );
            /* create the canonical buffer on the server's first MAP_SHARED map */
            if (!sm && (flags & MAP_SHARED) && !(flags & MAP_FIXED) && addr == NULL && g_n_shmaps < 64 && st.st_size > 0)
            {
                /* server-side (base=NULL) first map: allocate the canonical buffer */
                uint32_t fsize = (uint32_t)((st.st_size + 0xfff) & ~0xfffu);
                uint32_t base = bump_next;
                if (base + fsize <= GUEST_SPACE_END)
                {
                    ssize_t r = pread( fd, (void *)(uintptr_t)base, st.st_size, 0 );
                    if (r < 0) r = 0;
                    if ((uint32_t)r < fsize) memset( (char *)(uintptr_t)base + r, 0, fsize - r );
                    bump_next = base + fsize;
                    g_shmaps[g_n_shmaps].ino = st.st_ino;
                    g_shmaps[g_n_shmaps].base = base;
                    g_shmaps[g_n_shmaps].fsize = fsize;
                    sm = &g_shmaps[g_n_shmaps++];
                }
            }
            if (sm)
            {
                uint32_t canon = sm->base + (uint32_t)offset;
                if (!(flags & MAP_FIXED) && addr == NULL)
                {
                    if (vm_trace()) fprintf( stderr, "wasm_vm: shared-canon ino=%llu off=%llx -> %08x\n",
                                             (unsigned long long)st.st_ino, (long long)offset, canon );
                    return (void *)(uintptr_t)canon;   /* server view: the buffer itself */
                }
                /* client MAP_FIXED view: mirror the canonical buffer here */
                memcpy( (void *)(uintptr_t)a, (void *)(uintptr_t)canon, size );
                if (g_n_mirrors < 64)
                {
                    g_mirrors[g_n_mirrors].canon = canon;
                    g_mirrors[g_n_mirrors].addr = a;
                    g_mirrors[g_n_mirrors].size = size;
                    g_n_mirrors++;
                }
                if (vm_trace()) fprintf( stderr, "wasm_vm: shared-mirror ino=%llu off=%llx canon=%08x -> %08x\n",
                                         (unsigned long long)st.st_ino, (long long)offset, canon, a );
                return (void *)(uintptr_t)a;
            }
        }
    }

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
    {
        unsigned long long ino = 0;
        if (fd != -1) { struct stat st; if (fstat(fd,&st)==0) ino = (unsigned long long)st.st_ino; }
        fprintf( stderr, "wasm_vm: mmap %08x-%08x prot=%x flags=%x fd=%d off=%llx ino=%llu\n",
                 a, a + size, prot, flags, fd, (long long)offset, ino );
    }
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
