/*
 * Standalone i386 interpreter for native-wasm Wine (NO BoxedWine).
 *
 * This is the execution backend that plugs into ntdll's per-architecture seam
 * (normally signal_i386.c). It runs the guest's i386 user-mode code — the PE
 * ntdll.dll's loader, then the application — directly, with the wasm linear
 * memory as the flat guest address space (segment bases are 0). Native Wine is
 * re-entered when the guest calls one of the dispatcher trampolines, whose
 * addresses Wine stored into the guest PE (small wasm function-table indices),
 * so an EIP outside the guest code range routes back to C.
 *
 * Scope: a growing subset of user-mode integer i386. Unhandled opcodes log
 * their eip + bytes and stop, so coverage grows against real ntdll traces.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winnt.h"
#include "winternl.h"
#include "unix_private.h"

/* dispatcher trampoline addresses Wine stores into the guest (see loader.c
 * load_ntdll_functions). Their C symbols exist only so their ADDRESS is a
 * unique marker; the interpreter never calls them natively. */
extern void __wine_syscall_dispatcher(void);
extern void __wine_unix_call_dispatcher(void);

extern void *pLdrInitializeThunk;
extern void *pKiUserApcDispatcher;
extern void *pKiUserExceptionDispatcher;
extern void *pKiUserCallbackDispatcher;

/* ---- guest CPU state ---- */

enum { EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI };

struct x86cpu
{
    uint32_t regs[8];
    uint8_t  xmm[8][16];
    uint8_t  ymm[8][32];
    uint32_t eip;
    uint32_t eflags;
    uint32_t fs_base, gs_base;  /* i386 segment bases: fs -> TEB */
    /* lazy flags: result of the last flag-setting op */
    uint32_t lf_res;      /* result */
    uint32_t lf_op1, lf_op2;
    int      lf_size;     /* 1,2,4 bytes; 0 = flags are in eflags directly */
    int      lf_kind;     /* operation kind for CF/OF */
    int      lf_cin;      /* carry-in for K_ADC/K_SBB (0/1) */
    /* x87 FPU: register stack of 8 doubles, top-of-stack index, control/status */
    double   fpr[8];
    int      fptop;       /* st(i) == fpr[(fptop + i) & 7] */
    uint16_t fpcw;        /* control word */
    uint16_t fpsw;        /* status word (C3 C2 C1 C0 + top field) */
    int      running;
    int      exit_code;
};

/* eflags bits */
#define CF 0x0001
#define PF 0x0004
#define AF 0x0010
#define ZF 0x0040
#define SF 0x0080
#define DF 0x0400
#define OF 0x0800

/* flag-op kinds */
enum { K_NONE, K_ADD, K_SUB, K_LOGIC, K_INCDEC, K_ADC, K_SBB, K_INC, K_DEC };

static int x86_trace = -1;
static int trace(void)
{
    if (x86_trace < 0) { const char *e = getenv("WINEWASMX86TRACE"); x86_trace = e && *e && *e != '0'; }
    return x86_trace;
}

/* ---- flat memory (identity map) ---- */
uint64_t g_total_insns; /* total guest instructions executed (perf measurement) */
static int g_trace_3b = -1;
static uint64_t g_trace_3b_hits;
static uint32_t g_trace_3b_last;
static int g_trace_3b_detail = -1;
static unsigned g_trace_3b_detail_left;
/* GL-thunk bypass: skip the interpreted guest-side opengl32 marshalling by
 * calling the native gl_* unix thunk directly.  g_ogl_handle is opengl32's
 * unixlib funcs array, captured from the first GL unix-call (its return address
 * lands inside opengl32.dll).  Per hooked entry we stash the unix code + arg
 * count keyed by NAT_SLOT. */
#define NAT_SLOTS 2048
static uint32_t  g_ogl_base, g_ogl_size;
static uint32_t addr_unixcall;
static uintptr_t g_ogl_handle;
static uint32_t  g_wgl_swap_addr;
static uint16_t  g_gl_code[NAT_SLOTS];
static uint8_t   g_gl_nargs[NAT_SLOTS];
static int       g_gl_armed;
static int       g_glcount = -1;
static uint64_t  g_gl_calls[NAT_SLOTS];
static inline uint8_t  rd8 ( uint32_t a ){ return *(uint8_t  *)(uintptr_t)a; }
static inline uint16_t rd16( uint32_t a ){ return *(uint16_t *)(uintptr_t)a; }
static inline uint32_t rd32( uint32_t a ){ return *(uint32_t *)(uintptr_t)a; }
#ifdef WEBWINE_GENBLOCKS
/* Undo log for the JIT differential verify: while recording, every guest write
 * saves the old value so the harness can roll the block's memory back and re-run
 * it through the interpreter from an identical state.  Zero cost when not the
 * GENBLK build; one predictable branch (recording off) when it is. */
#define WLOG_N 1024
static struct { uint32_t addr, old; uint8_t size; } g_wlog[WLOG_N];
static int g_wlog_n, g_jit_recording, g_wlog_of;
static inline void wlog( uint32_t a, uint32_t old, uint8_t sz )
{ if (g_wlog_n < WLOG_N) { g_wlog[g_wlog_n].addr=a; g_wlog[g_wlog_n].old=old; g_wlog[g_wlog_n].size=sz; g_wlog_n++; } else g_wlog_of = 1; }
static inline void wr8 ( uint32_t a, uint8_t  v ){ if (g_jit_recording) wlog(a, *(uint8_t *)(uintptr_t)a, 1); *(uint8_t  *)(uintptr_t)a = v; }
static inline void wr16( uint32_t a, uint16_t v ){ if (g_jit_recording) wlog(a, *(uint16_t*)(uintptr_t)a, 2); *(uint16_t *)(uintptr_t)a = v; }
static inline void wr32( uint32_t a, uint32_t v ){ if (g_jit_recording) wlog(a, *(uint32_t*)(uintptr_t)a, 4); *(uint32_t *)(uintptr_t)a = v; }
#else
static inline void wr8 ( uint32_t a, uint8_t  v ){ *(uint8_t  *)(uintptr_t)a = v; }
static inline void wr16( uint32_t a, uint16_t v ){ *(uint16_t *)(uintptr_t)a = v; }
static inline void wr32( uint32_t a, uint32_t v ){ *(uint32_t *)(uintptr_t)a = v; }
#endif

/* ---- headless frame capture ----------------------------------------------
 * netduke32 renders its 8-bpp Build frame, then de-palettises it to a 16-bpp
 * RGB565 staging buffer via softsurface_blitBufferInternal<uint16_t> before
 * handing it to SDL_UpdateTexture.  With no display backend we intercept that
 * function (EAX = destination buffer) and dump the RGB565 frame as a PPM so
 * the running game is visibly demonstrable.  Env-gated:
 *   WASM_DUMP_FRAME    = output PPM path (enables capture)
 *   WASM_DUMP_FRAME_N  = which blit call to dump (default 60; lets the attract
 *                        loop advance past black startup frames)
 * The blit-internal address and the width/height globals are read from the
 * supplied netduke32.exe disassembly (see webwine-netduke32-runs memory).   */
#define ND_VIDEONEXTPAGE 0x519620u  /* _videoNextPage — page flip (frame done) */
#define ND_FRAMEPLACE    0xe168bcu  /* uint8_t* current 8-bpp frame buffer     */
#define ND_BYTESPERLINE  0x10d3b40u /* stride in bytes                         */
#define ND_XDIM          0x93004cu  /* screen width                            */
#define ND_YDIM          0x930048u  /* screen height                           */
#define ND_CURPALETTE    0xa61220u  /* palette_t[256], 4 bytes/entry           */
static int32_t nd_slide = 0;   /* actual_exe_base - 0x400000 (ASLR relocation) */
static int32_t msvcrt_slide = 0; /* loaded msvcrt base - PE preferred 0x10000000 */
static uint32_t msvcrt_base;
static int g_present_count = 0;
static uint64_t g_flip_count = 0;   /* # of _videoNextPage calls = real game frames */
static uint32_t g_flip_addr = 0;    /* ND_VIDEONEXTPAGE + slide, set once slide known (0 = never matches, eip>=0x10000) */
static int g_slide_ok = 0;          /* ASLR slide derived from PEB */
/* Last non-zero frameplace + dims.  The engine clears `frameplace` to 0 in
 * videoEndDrawing before the page flip, but the buffer it pointed at still holds
 * the finished frame, and the pointer is constant across a frame's drawing — so
 * caching it lets us present a COMPLETE frame at the flip instead of an empty 0. */
static uint32_t g_last_fp = 0, g_last_bpl = 0;
static int g_last_w = 0, g_last_h = 0;
static uint64_t g_histo[512];       /* opcode histogram (env WASM_HISTO): [op], [256+op2] */
static int g_histo_on = 0;

static int browser_fast_render( void )
{
#ifdef WEBWINE_BROWSER
    static int fast = -1;
    if (fast < 0) fast = getenv( "WASM_FAST_RENDER" ) ? 1 : 0;
    return fast;
#else
    return 1;
#endif
}

static int browser_fast_columns( void )
{
#ifdef WEBWINE_BROWSER
    static int fast = -1;
    if (fast < 0) fast = getenv( "WASM_NO_FAST_COLUMNS" ) ? 0 : 1;
    return fast;
#else
    return 1;
#endif
}

static int browser_fast_masked_columns( void )
{
#ifdef WEBWINE_BROWSER
    static int fast = -1;
    if (fast < 0) fast = getenv( "WASM_FAST_MVLINE" ) && !getenv( "WASM_NO_FAST_COLUMNS" );
    return fast;
#else
    return 1;
#endif
}

static int browser_fast_libdiv( void )
{
#ifdef WEBWINE_BROWSER
    static int fast = -1;
    if (fast < 0) fast = getenv( "WASM_FAST_LIBDIV" ) && !getenv( "WASM_NO_FAST_LIBDIV" );
    return fast;
#else
    return 1;
#endif
}

static int nd_dynamic_con_addr( uint32_t addr )
{
    return addr >= 0x03000000u && addr < 0x04000000u;
}

/* Guest eip sampling profiler (env WASM_PROF).  Samples are taken on the
 * existing ~64K-instruction housekeeping tick, so the hot path pays NOTHING and
 * the sample is uniform over instructions executed.  Addresses are dumped raw;
 * map them to function names offline with the exe's symbol table
 * (VA = symbol value + 0x401000).  This is how you find out which guest code
 * actually costs the frame - e.g. whether the engine's present/blit chain is
 * burning instructions we do not need, since we read frameplace directly. */
#define PROF_SLOTS 4096
static uint32_t g_prof_eip[PROF_SLOTS];
static uint32_t g_prof_cnt[PROF_SLOTS];
static int g_prof_on = 0;
static int g_prof_countdown = 1;
static uint64_t g_prof_total, g_prof_dropped;   /* true totals: the table + top-N list only show the peaks */
static void prof_sample( uint32_t eip )
{
    uint32_t h = (eip * 2654435761u) >> 20;      /* fibonacci hash -> 12 bits */
    g_prof_total++;
    for (int i = 0; i < 8; i++)                  /* short linear probe */
    {
        uint32_t s = (h + i) & (PROF_SLOTS - 1);
        if (g_prof_cnt[s] == 0) { g_prof_eip[s] = eip; g_prof_cnt[s] = 1; return; }
        if (g_prof_eip[s] == eip) { g_prof_cnt[s]++; return; }
    }
    g_prof_dropped++;   /* table pressure: drop rather than evict */
}
/* Identify which PE module an address belongs to: scan down for the MZ/PE
 * header (modules are 64K-aligned) and read the export directory's name.  Used
 * only by the profiler dump, so cost does not matter. */
static const char *prof_module( uint32_t va )
{
    for (uint32_t base = va & ~0xffffu; base >= 0x10000u; base -= 0x10000u)
    {
        if (rd16( base ) != 0x5a4d) continue;                  /* 'MZ' */
        uint32_t pe = base + rd32( base + 0x3c );
        if (pe < base || pe > base + 0x1000 || rd32( pe ) != 0x00004550) continue;  /* 'PE\0\0' */
        uint32_t exp = rd32( pe + 0x78 );                      /* export dir RVA */
        if (!exp) return "(module)";
        static char buf[128];
        unsigned i = 0;
        uint32_t nm = rd32( base + exp + 0x0c );
        if (nm) { uint32_t p = base + nm;
                  while (i < 40) { uint8_t ch = rd8( p + i ); if (!ch) break; buf[i++] = (char)ch; } }
        buf[i] = 0;
        /* nearest preceding export, so the hot address gets a function name */
        uint32_t nfun = rd32( base + exp + 0x14 ), nnam = rd32( base + exp + 0x18 );
        uint32_t afun = rd32( base + exp + 0x1c ), anam = rd32( base + exp + 0x20 ), aord = rd32( base + exp + 0x24 );
        uint32_t rva = va - base, bestrva = 0, bestname = 0;
        if (nfun && nnam && afun && anam && aord)
            for (uint32_t k = 0; k < nnam && k < 4000; k++)
            {
                uint16_t ord = rd16( base + aord + k * 2 );
                if (ord >= nfun) continue;
                uint32_t f = rd32( base + afun + ord * 4 );
                if (f <= rva && f > bestrva) { bestrva = f; bestname = rd32( base + anam + k * 4 ); }
            }
        if (bestname)
        {
            unsigned j = i;
            if (j < sizeof(buf) - 2) buf[j++] = '!';
            uint32_t p = base + bestname;
            while (j < sizeof(buf) - 12) { uint8_t ch = rd8( p + j - i - 1 ); if (!ch) break; buf[j++] = (char)ch; }
            snprintf( buf + j, sizeof(buf) - j, "+0x%x", (unsigned)(rva - bestrva) );
        }
        return buf;
    }
    return "(unknown)";
}

/* ---- native acceleration of Wine's own CRT block moves ----
 *
 * Profiling netduke32 showed ~69% of all guest instructions inside
 * msvcrt.dll!memmove's inner copy loop: the engine blits its framebuffer every
 * frame, and we were interpreting that byte-shuffling one x86 instruction at a
 * time.  msvcrt is OUR builtin, not the application, so running its block moves
 * natively is the same kind of shortcut Wine takes when it uses the host's
 * optimized memcpy - the guest-visible semantics are identical.
 *
 * Entry points are found once from the PEB loader list and registered in a
 * direct-mapped table, so the hot path costs one indexed load + compare, the
 * same as the frame-flip check it replaces (run() is register-pressure bound,
 * so it must not get more expensive than that). */
enum { NAT_FLIP = 1, NAT_MEMMOVE, NAT_MEMSET, NAT_COUNT, NAT_AGELOOP,
       NAT_SDL_OPEN, NAT_SDL_OPENDEV, NAT_SDL_PAUSE, NAT_SDL_PAUSEDEV,
       NAT_SDL_LOCK, NAT_SDL_UNLOCK, NAT_SDL_CLOSE, NAT_VLINE, NAT_VLINE_DISPATCH, NAT_MVLINE_DISPATCH, NAT_SURFBLIT,
       NAT_SDL_POLL, NAT_SDL_RELMOUSE, NAT_SDL_KEYBOARDSTATE, NAT_MVLINE, NAT_SURFSPAN, NAT_LIBDIV, NAT_MHLINE, NAT_GLSTATE, NAT_GLSAMPLER, NAT_VLINE1, NAT_MVLINE1, NAT_CRC32, NAT_PALMATCH, NAT_MIXSTEREO, NAT_MIDEBUG, NAT_GLTHUNK, NAT_AGEBLOCKS, NAT_QRHLINE,
       NAT_MEMCMP, NAT_STRCMP, NAT_STRLEN, NAT_MEMCHR, NAT_STRncmp,
       NAT_STRCHR, NAT_STRCPY, NAT_STRNCPY, NAT_WCSLEN, NAT_WCSCHR, NAT_MEMSET_LOOP, NAT_TOASCII, NAT_TOLOWER, NAT_TOUPPER, NAT_FLOOR, NAT_QPF, NAT_QPC, NAT_GETTID, NAT_TLSGETVALUE,
       NAT_VLINE1NP2, NAT_MVLINE1NP2, NAT_UDIVMODDI4, NAT_SDL_TLSGET, NAT_SDL_ATOMIC_GET,
       NAT_SDL_ATOMIC_GETPTR, NAT_SDL_ATOMIC_XADD, NAT_NTDLL_SRW_EXCL,
       NAT_NTDLL_SRW_SHARED, NAT_NTDLL_SRW_EXCL_REL, NAT_NTDLL_WAKE_ALL_EMPTY,
       NAT_GLTHUNK_RET, NAT_SETUPQRHLINE, NAT_POW, NAT_WIN_GL_SWAP,
       NAT_PTHREAD_SPIN_UNLOCK, NAT_PTHREAD_SPIN_LOCK, NAT_PTHREAD_GETSPECIFIC,
       NAT_GDI_RELAY, NAT_GDI_CLIENT, NAT_SETUP_VLINE, NAT_SETUP_PVLINE, NAT_SETUP_MVLINE,
       NAT_SETUP_TVLINE };
static uint64_t g_count_hits;   /* diagnostic: executions of a watched address */
static uint64_t g_vl_calls, g_vl_iters;   /* native vlineasm4: entries and loop iterations */
static uint64_t g_sb_calls, g_sb_iters;   /* native surface blit: entries and iterations */
static uint64_t g_mv_calls, g_mv_iters;   /* native mvlineasm4: entries and loop iterations */
static uint32_t g_nat_addr[NAT_SLOTS];
static uint8_t  g_nat_kind[NAT_SLOTS];
static int g_nat_ready = 0;
static uint32_t g_ntdll_wake_buckets;
static uint32_t g_msvcrt_memset_loop;
#if defined(WEBWINE_GENBLOCKS) && defined(WEBWINE_MSVCRT_AOT)
static int g_msvcrt_jit;
static void msvcrt_gen_build_hash( void );
#endif
#ifdef WEBWINE_GENBLOCKS
/* GDI32 is relocatable too; keep this experimental table separate from the
 * executable table because its image base is chosen independently by Wine. */
static int32_t gdi32_slide;
static int gdi32_jit, gdi32_gen_loaded;
static int gdi32_relay_armed;
static uint32_t g_gdi_client_teb, g_gdi_client_shared;
static void gdi32_gen_build_hash( void );
#endif
#define NAT_SLOT(a) (((a) >> 4) & (NAT_SLOTS - 1))

static void nat_register( uint32_t addr, int kind, const char *what )
{
    if (!addr) return;
    unsigned s = NAT_SLOT( addr );
    if (g_nat_addr[s]) { fprintf( stderr, "wasm_x86: native %s: slot busy, skipped\n", what ); return; }
    g_nat_addr[s] = addr; g_nat_kind[s] = (uint8_t)kind;
    fprintf( stderr, "wasm_x86: native %s @ %08x\n", what, addr );
}

#ifdef WEBWINE_GENBLOCKS
extern int wasm_x86_dispatch( struct x86cpu *c, uint32_t target );
/* Wine's builtin PE DLLs begin exported calls with a generated relay thunk.
 * The thunk is deliberately boring but very hot: it pushes the relay index
 * and descriptor, calls the native relay dispatcher, then executes a stdcall
 * ret.  Keep the actual DLL implementation and dispatcher untouched; this
 * shortcut only removes the fixed guest instructions around that call. */
static int nat_gdi_relay( struct x86cpu *c )
{
    uint32_t b = c->eip, table, target;
    if (rd8(b + 6) != 0x68 || rd8(b + 11) != 0xb8 || rd8(b + 16) != 0x50 ||
        rd8(b + 17) != 0xff || rd8(b + 18) != 0x50 || rd8(b + 19) != 0x04 ||
        rd8(b + 20) != 0xc2) return 0;
    table = rd32( b + 12 );
    if (!table || table >= 0x70000000u - 8u) return 0;
    target = rd32( table + 4 );
    if (!target || target >= 0x10000u) return 0;
    c->regs[EAX] = table;                 /* mov eax, descriptor */
    c->regs[ESP] -= 4; wr32( c->regs[ESP], rd32( b + 7 ) ); /* relay ordinal */
    c->regs[ESP] -= 4; wr32( c->regs[ESP], table );
    c->regs[ESP] -= 4; wr32( c->regs[ESP], b + 20 );        /* call continuation */
    c->eip = target;                      /* native relay trampoline */
    if (target == addr_unixcall)          /* skip one interpreter dispatch turn */
        return wasm_x86_dispatch( c, target );
    return 1;
}

static void nat_arm_gdi_relays( uint32_t base )
{
    unsigned armed = 0, busy = 0;
    if (gdi32_relay_armed) return;
    gdi32_relay_armed = 1;
    if (!getenv( "WASM_GDI32_RELAY" )) return;
    /* This range is the generated gdi32 relay-entry cluster.  Verify every
     * candidate before placing it in the native table; the DLL body starts
     * after 0x582c and is never covered by this shortcut. */
    for (uint32_t rva = 0x1a4c; rva < 0x582c; rva += 4)
    {
        uint32_t b = base + rva;
        if (rd8(b) != 0x8b || rd8(b+1) != 0xff || rd8(b+2) != 0x55 ||
            rd8(b+3) != 0x8b || rd8(b+4) != 0xec || rd8(b+5) != 0x5d ||
            rd8(b+6) != 0x68 || rd8(b+11) != 0xb8 || rd8(b+16) != 0x50 ||
            rd8(b+17) != 0xff || rd8(b+18) != 0x50 || rd8(b+19) != 0x04 ||
            rd8(b+20) != 0xc2) continue;
        unsigned s = NAT_SLOT(b);
        if (g_nat_addr[s]) { busy++; continue; }
        g_nat_addr[s] = b; g_nat_kind[s] = NAT_GDI_RELAY; armed++;
    }
    fprintf( stderr, "wasm_x86: native gdi32 relay thunks armed=%u busy=%u\n", armed, busy );
}

/* gdi32's private get_gdi_client_ptr() is on the software-renderer's hot
 * handle path.  Its normal i386 path is a bounds/type/generation check in the
 * PEB's shared GDI table followed by one UserPointer load.  Keep the unusual
 * TEB64/GdiBatchCount path and invalid handles in the guest: those paths also
 * carry diagnostics, whereas the common path below has no observable side
 * effect beyond EAX and the return address.
 *
 * This is opt-in until the differential run has covered handle creation and
 * teardown.  The exact function skeleton is checked when the DLL is loaded,
 * so a Wine update cannot silently apply the layout assumption to a different
 * implementation. */
static int nat_gdi_client_ptr( struct x86cpu *c )
{
    uint32_t sp = c->regs[ESP], teb, teb64, peb, shared, obj, type, entry, unique, ret;
    uint32_t idx;
    if (sp >= 0x70000000u - 16u) return 0;
    teb = c->fs_base;
    if (!teb || teb >= 0x70000000u - 0xf74u) return 0;
    if (g_gdi_client_teb == teb && g_gdi_client_shared)
        shared = g_gdi_client_shared;
    else
    {
        teb64 = rd32(teb + 0xf70u);
        if (teb64)
        {
            /* The i386 Wine DLL uses TEB64+0x60 -> PEB64 and PEB64+0xf8 for
             * GdiSharedHandleTable when the WoW64 compatibility TEB is present. */
            if (teb64 >= 0x70000000u - 0x64u) return 0;
            peb = rd32(teb64 + 0x60u);
            if (!peb || peb >= 0x70000000u - 0xfcu) return 0;
            shared = rd32(peb + 0xf8u);
        }
        else
        {
            peb = rd32(teb + 0x30u);
            if (!peb || peb >= 0x70000000u - 0x98u) return 0;
            shared = rd32(peb + 0x94u);
        }
        if (shared) { g_gdi_client_teb = teb; g_gdi_client_shared = shared; }
    }
    if (!shared || shared >= 0x70000000u - 0x100000u) return 0;
    /* The dispatch seam is immediately after the function's initial
     * push ebp (the loader may enter at the hotpatch byte, but the
     * interpreter advances once before the native check).  Thus [sp+4] is
     * the caller return address and the two arguments start at [sp+8]. */
    obj = rd32(sp + 8u); type = rd32(sp + 12u); idx = obj & 0xffffu;
    ret = rd32(sp + 4u);
    /* A few loader-generated calls do not have the ordinary internal-call
     * frame at this seam.  A return into the guest stack is never valid here;
     * decline before touching ESP and let the already-entered interpreter
     * body handle it. */
    if (ret < 0x400000u || ret >= 0x70000000u) return 0;
    entry = shared + idx * 24u;
    if (idx >= 0x10000u || entry < shared || entry >= 0x70000000u - 0x18u)
        return 0;
    if (!rd8(entry + 0x0eu)) return 0; /* invalid: let Wine emit its warning */
    unique = rd16(entry + 0x0cu);
    if ((obj >> 16) && (obj >> 16) != unique) return 0;
    if (type && (((uint32_t)(rd8(entry + 0x0du) & 0x7fu) << 16) != type))
        return 0;
    c->regs[EAX] = rd32(entry + 0x10u);
    c->regs[ESP] = sp + 8u; /* leave restores EBP, then ret pops [sp+4] */
    c->eip = ret;
    return 1;
}

static void nat_arm_gdi_client_ptr( uint32_t base )
{
    static const uint8_t head[] = {
        0x64,0x8b,0x15,0x18,0x00,0x00,0x00,0x89,0xe5,0x53,0x83,0xec,0x14,
        0x8b,0x9a,0x70,0x0f,0x00,0x00,0x8b,0x45,0x08,0x8b,0x4d,0x0c
    };
    uint32_t a = base + 0x4abc1u;
    unsigned i;
    if (!getenv( "WASM_GDI_CLIENT" )) return;
    for (i = 0; i < sizeof(head); i++)
        if (rd8(a + i) != head[i]) break;
    if (i == sizeof(head)) nat_register(a, NAT_GDI_CLIENT, "gdi32 get_gdi_client_ptr");
    else fprintf(stderr, "wasm_x86: gdi32 get_gdi_client_ptr skeleton differs at %08x+%x - left interpreted\n", a, i);
}
#endif

/* Resolve one export by name from a loaded PE image. */
static uint32_t pe_export( uint32_t base, const char *want )
{
    if (rd16( base ) != 0x5a4d) return 0;
    uint32_t pe = base + rd32( base + 0x3c );
    if (rd32( pe ) != 0x00004550) return 0;
    uint32_t exp = rd32( pe + 0x78 );
    if (!exp) return 0;
    uint32_t nnam = rd32( base + exp + 0x18 ), afun = rd32( base + exp + 0x1c );
    uint32_t anam = rd32( base + exp + 0x20 ), aord = rd32( base + exp + 0x24 );
    if (!nnam || !afun || !anam || !aord) return 0;
    for (uint32_t k = 0; k < nnam; k++)
    {
        uint32_t p = base + rd32( base + anam + k * 4 );
        unsigned i = 0;
        while (want[i] && rd8( p + i ) == (uint8_t)want[i]) i++;
        if (want[i] || rd8( p + i )) continue;          /* full match only */
        return base + rd32( base + afun + rd16( base + aord + k * 2 ) * 4 );
    }
    return 0;
}

/* Walk PEB->Ldr->InMemoryOrderModuleList for a module by (lowercased) name. */
static uint32_t find_module( struct x86cpu *c, const char *want )
{
    uint32_t teb = c->fs_base;
    uint32_t peb = teb ? rd32( teb + 0x30 ) : 0;
    uint32_t ldr = peb ? rd32( peb + 0x0c ) : 0;
    if (!ldr) return 0;
    uint32_t head = ldr + 0x14, cur = rd32( head );
    for (int n = 0; n < 256 && cur && cur != head; n++, cur = rd32( cur ))
    {
        uint32_t ent = cur - 0x08;                       /* InMemoryOrderLinks */
        uint32_t dllbase = rd32( ent + 0x18 );
        uint16_t len = rd16( ent + 0x2c );
        uint32_t buf = rd32( ent + 0x30 );
        if (!dllbase || !buf || !len) continue;
        unsigned i = 0;
        for (; want[i] && i < len / 2; i++)
        {
            uint16_t wc = rd16( buf + i * 2 );
            if (wc >= 'A' && wc <= 'Z') wc += 32;
            if (wc != (uint16_t)want[i]) break;
        }
        if (!want[i] && i == len / 2) return dllbase;
    }
    return 0;
}

/* One-shot dump of every loaded guest module (name + base) - identifies which
 * DLL owns a hot interpreted address range.  Gated by WASM_MODULES. */
static int dump_modules( struct x86cpu *c )
{
    uint32_t teb = c->fs_base;
    uint32_t peb = teb ? rd32( teb + 0x30 ) : 0;
    uint32_t ldr = peb ? rd32( peb + 0x0c ) : 0;
    if (!ldr) return 0;
    uint32_t head = ldr + 0x14, cur = rd32( head );
    int cnt = 0;
    for (int n = 0; n < 256 && cur && cur != head; n++, cur = rd32( cur ))
    {
        uint32_t ent = cur - 0x08;
        uint32_t dllbase = rd32( ent + 0x18 ), esz = rd32( ent + 0x20 );
        uint16_t len = rd16( ent + 0x2c ); uint32_t buf = rd32( ent + 0x30 );
        if (!dllbase) continue;
        char name[128]; unsigned i = 0;
        for (; i < len/2 && i < sizeof(name)-1; i++) name[i] = (char)rd16( buf + i*2 );
        name[i] = 0;
        fprintf( stderr, "MODULE %08x-%08x %s\n", dllbase, dllbase + esz, name );
        cnt++;
    }
    return cnt;
}

static void nat_init( struct x86cpu *c )
{
    /* msvcrt is loaded well after the exe base is known, so keep retrying on
     * later ticks rather than giving up on the first look. */
    uint32_t base = find_module( c, "msvcrt.dll" );
#ifdef WEBWINE_GENBLOCKS
    /* GDI32 can enter the loader list after msvcrt.  Do not consume the
     * one-shot initialization attempt while it is still absent: the relay
     * cluster is a measured frame hotspot, and a late-loaded DLL must still
     * get its relocatable base and verified hooks. */
    if (!gdi32_gen_loaded)
    {
        uint32_t gbase = find_module( c, "gdi32.dll" );
        if (gbase)
        {
            gdi32_slide = (int32_t)(gbase - 0x10000000u);
            gdi32_jit = getenv( "WASM_GDI32_JIT" ) ? 1 : 0;
            gdi32_gen_loaded = 1;
            if (gdi32_jit) gdi32_gen_build_hash();
            nat_arm_gdi_relays( gbase );
            nat_arm_gdi_client_ptr( gbase );
        }
    }
#endif
    if (!base) return;
    msvcrt_base = base;
    msvcrt_slide = (int32_t)(base - 0x10000000u);
#if defined(WEBWINE_GENBLOCKS) && defined(WEBWINE_MSVCRT_AOT)
    /* This first DLL AOT pass is opt-in until its full PE ABI surface has
     * differential coverage.  The generated table is kept available for
     * focused testing, while normal builds retain the proven native CRT hooks
     * and interpreter fallback. */
    g_msvcrt_jit =
#ifdef WEBWINE_MSVCRT_FOCUSED_AOT
        getenv( "WASM_NO_MSVCRT_JIT" ) ? 0 : 1;
#else
        getenv( "WASM_MSVCRT_JIT" ) ? 1 : 0;
#endif
    if (g_msvcrt_jit) msvcrt_gen_build_hash();
#endif
    g_nat_ready = 1;
    nat_register( pe_export( base, "memmove" ), NAT_MEMMOVE, "memmove" );
    nat_register( pe_export( base, "memcpy" ),  NAT_MEMMOVE, "memcpy" );
    nat_register( pe_export( base, "memset" ),  NAT_MEMSET,  "memset" );
    { uint32_t loop = base + 0x49b80u;
      static const uint8_t code[] = { 0x0f,0x29,0x06,0x83,0xc6,0x20,
                                      0x0f,0x29,0x46,0xf0,0x39,0xde,0x72,0xf2 };
      unsigned i; int ok = !getenv( "WASM_NO_MEMSET_LOOP" );
      for (i = 0; ok && i < sizeof(code); i++) if (rd8( loop + i ) != code[i]) ok = 0;
      if (ok) { g_msvcrt_memset_loop = loop; nat_register( loop, NAT_MEMSET_LOOP, "msvcrt memset SIMD loop" ); }
      else if (!getenv( "WASM_NO_MEMSET_LOOP" ))
          fprintf( stderr, "wasm_x86: msvcrt memset loop differs at %08x - left interpreted\n", loop );
    }
    nat_register( pe_export( base, "memcmp" ),  NAT_MEMCMP,  "memcmp" );
    nat_register( pe_export( base, "strcmp" ),  NAT_STRCMP,  "strcmp" );
    nat_register( pe_export( base, "strlen" ),  NAT_STRLEN,  "strlen" );
    nat_register( pe_export( base, "memchr" ),  NAT_MEMCHR,  "memchr" );
    nat_register( pe_export( base, "strncmp" ), NAT_STRncmp,  "strncmp" );
    nat_register( pe_export( base, "strchr" ),  NAT_STRCHR,  "strchr" );
    nat_register( pe_export( base, "strcpy" ),  NAT_STRCPY,  "strcpy" );
    nat_register( pe_export( base, "strncpy" ), NAT_STRNCPY, "strncpy" );
    nat_register( pe_export( base, "__toascii" ), NAT_TOASCII, "__toascii" );
    nat_register( pe_export( base, "tolower" ), NAT_TOLOWER, "tolower" );
    nat_register( pe_export( base, "toupper" ), NAT_TOUPPER, "toupper" );
    /* Non-exported GCC helper used by libdivide's 64-bit arithmetic. */
    { uint32_t udiv = base + 0x100f0u;
      static const uint8_t head[] = { 0x55,0x89,0xe5,0x57,0x56,0x53,
                                      0x83,0xe4,0xf8,0x83,0xec,0x20 };
      int ok = !getenv( "WASM_NO_UDIV_NATIVE" );
      for (unsigned i = 0; ok && i < sizeof(head); i++)
          if (rd8( udiv + i ) != head[i]) ok = 0;
      if (ok) nat_register( udiv, NAT_UDIVMODDI4, "__udivmoddi4" );
      else if (!getenv( "WASM_NO_UDIV_NATIVE" ))
          fprintf( stderr, "wasm_x86: __udivmoddi4 skeleton differs at %08x - left interpreted\n", udiv );
    }
    { uint32_t adj = pe_export( base, "_adj_fptan" );
      uint32_t b = adj; int ok = adj != 0;
      /* Wine's i386 implementation is a trace-only Pentium workaround stub:
       * test a debug flag, optionally emit TRACE, then return.  Bypass that
       * wrapper only when its stable test/branch/return skeleton matches. */
      static const uint8_t head[] = { 0xf6,0x05,0,0,0,0,0x08,0x75,0x07 };
      for (unsigned i = 0; ok && i < sizeof(head); i++)
          if (head[i] && rd8( b + i ) != head[i]) ok = 0;
      if (ok && rd8(b+9) != 0xc3 && rd8(b+16) != 0xc3) ok = 0;
      if (ok) nat_register( adj, NAT_MIDEBUG, "_adj_fptan (stub)" );
      else if (adj) fprintf( stderr, "wasm_x86: _adj_fptan skeleton differs at %08x - left interpreted\n", adj );
    }
    base = find_module( c, "ntdll.dll" );
    /* NTDLL carries the same cdecl byte-copy ABI as msvcrt.  These exports are
     * used by Wine's loader and allocator paths, and the existing nat_call
     * bounds checks make the host implementations byte-exact for guest memory.
     * Keep the hooks separate from the msvcrt addresses: the two DLLs can be
     * loaded at unrelated bases and their slot identities must not collide. */
    nat_register( pe_export( base, "memcpy" ),  NAT_MEMMOVE, "ntdll memcpy" );
    nat_register( pe_export( base, "memmove" ), NAT_MEMMOVE, "ntdll memmove" );
    nat_register( pe_export( base, "memset" ),  NAT_MEMSET,  "ntdll memset" );
    nat_register( pe_export( base, "wcslen" ), NAT_WCSLEN, "wcslen" );
    { uint32_t qpf = pe_export( base, "RtlQueryPerformanceFrequency" );
      int ok = qpf != 0;
      { uint32_t b = qpf + (uint32_t)nd_slide;
        static const uint8_t head[] = { 0x8b,0xff,0x55,0x8b,0xec,0x8b,0x45,0x08 };
        for (unsigned i = 0; ok && i < sizeof(head); i++)
            if (rd8( b + i ) != head[i]) ok = 0;
        /* The two stores and their immediate values are the semantic body;
         * allow harmless hotpatch/padding differences in the surrounding
         * compiler-generated sequence. */
        if (ok && (rd8(b+8)!=0xc7 || rd8(b+9)!=0x00 || rd8(b+10)!=0x80 ||
                   rd8(b+11)!=0x96 || rd8(b+12)!=0x98 || rd8(b+13)!=0x00 ||
                   rd8(b+14)!=0xc7 || rd8(b+15)!=0x40 || rd8(b+16)!=0x04 ||
                   rd32(b+17)!=0)) ok = 0;
        /* Keep the export identity and store sequence as the guard.  The
         * trailing return sequence is allowed to differ across Wine builds;
         * nat_qpf supplies the same stdcall result and stack cleanup. */
      }
      if (ok) nat_register( qpf, NAT_QPF, "RtlQueryPerformanceFrequency" );
      else if (qpf) fprintf( stderr, "wasm_x86: RtlQueryPerformanceFrequency skeleton differs at %08x - left interpreted\n", qpf );
    }
    { uint32_t qpc = pe_export( base, "RtlQueryPerformanceCounter" );
      static const uint8_t head[] = { 0x8b,0xff,0x55,0x8b,0xec,0x83,0xec,0x08,
                                      0x8b,0x45,0x08,0xc7,0x44,0x24 };
      uint32_t b = qpc + (uint32_t)nd_slide; int ok = qpc != 0;
      for (unsigned i = 0; ok && i < sizeof(head); i++)
          if (rd8( b + i ) != head[i]) ok = 0;
      /* The call displacement is relocation-dependent.  The return value and
       * ret 4 are stable and bound the helper we replace. */
      if (ok && (rd8(b+27)!=0xb8 || rd32(b+28)!=1 || rd8(b+36)!=0xc2 || rd16(b+37)!=4)) ok = 0;
      if (ok) nat_register( qpc, NAT_QPC, "RtlQueryPerformanceCounter" );
      else if (qpc) fprintf( stderr, "wasm_x86: RtlQueryPerformanceCounter skeleton differs at %08x - left interpreted\n", qpc );
    }
    { uint32_t srw = pe_export( base, "RtlAcquireSRWLockExclusive" );
      /* Wine's i386 implementation first adds two to the waiter field, then
       * CASes the unchanged waiter field plus the exclusive bit.  When the
       * waiter field is zero, the net effect is one atomic dword transition;
       * the native path below performs exactly that uncontended transition.
       * Keep the guard tied to the implementation's entry and lock-add
       * skeleton, and leave every contended/unrecognised state interpreted. */
      static const uint8_t head[] = { 0x55,0x89,0xe5,0x57,0x56,0x53,0x83,0xec,
                                      0x14,0x8b,0x7d,0x08,0x66,0xf0,0x83,0x07,0x02 };
      uint32_t b = srw + (uint32_t)nd_slide; int ok = srw != 0;
      for (unsigned i = 0; ok && i < sizeof(head); i++)
          if (rd8( b + i ) != head[i]) ok = 0;
      if (ok && (rd8(b+0x40)!=0x8b || rd8(b+0x41)!=0x17 ||
                 rd8(b+0x42)!=0x89 || rd8(b+0x43)!=0xd0 ||
                 rd8(b+0x44)!=0x89 || rd8(b+0x45)!=0x55 || rd8(b+0x46)!=0xf0)) ok = 0;
      if (ok && !getenv( "WASM_NO_NTDLL_SRW_FAST" ))
          nat_register( srw, NAT_NTDLL_SRW_EXCL, "ntdll RtlAcquireSRWLockExclusive fast path" );
      else if (srw && !getenv( "WASM_NO_NTDLL_SRW_FAST" ))
          fprintf( stderr, "wasm_x86: RtlAcquireSRWLockExclusive skeleton differs at %08x - left interpreted\n", srw );
    }
    { uint32_t srw = pe_export( base, "RtlAcquireSRWLockShared" );
      /* Shared acquisition increments the high half-word only when the low
       * half-word (exclusive bit plus waiter state) is zero.  A single CAS is
       * therefore the exact uncontended operation; a nonzero low half-word or
       * a CAS race returns to Wine's wait protocol. */
      static const uint8_t head[] = { 0x55,0x89,0xe5,0x56,0x53,0x8d,0x75,0xf4,
                                      0x83,0xec,0x14,0x8b,0x5d,0x08 };
      uint32_t b = srw + (uint32_t)nd_slide; int ok = srw != 0;
      for (unsigned i = 0; ok && i < sizeof(head); i++)
          if (rd8( b + i ) != head[i]) ok = 0;
      if (ok && !getenv( "WASM_NO_NTDLL_SRW_FAST" ))
          nat_register( srw, NAT_NTDLL_SRW_SHARED, "ntdll RtlAcquireSRWLockShared fast path" );
      else if (srw && !getenv( "WASM_NO_NTDLL_SRW_FAST" ))
          fprintf( stderr, "wasm_x86: RtlAcquireSRWLockShared skeleton differs at %08x - left interpreted\n", srw );
    }
    { uint32_t srw = pe_export( base, "RtlReleaseSRWLockExclusive" );
      /* Wine stores owners in the high half-word and the exclusive ownership
       * bit in the low half-word.  With no waiters, release is exactly the
       * uncontended atomic transition 0x00010001 -> 0.  Any other state is
       * left to Wine so its waiter wake protocol remains authoritative. */
      static const uint8_t head[] = { 0x55,0x89,0xe5,0x56,0x53,0x83,0xec,0x14,
                                      0x8b,0x75,0x08,0xeb,0x25 };
      uint32_t b = srw + (uint32_t)nd_slide; int ok = srw != 0;
      for (unsigned i = 0; ok && i < sizeof(head); i++)
          if (rd8( b + i ) != head[i]) ok = 0;
      if (ok && !getenv( "WASM_NO_NTDLL_SRW_FAST" ))
          nat_register( srw, NAT_NTDLL_SRW_EXCL_REL,
                        "ntdll RtlReleaseSRWLockExclusive uncontended fast path" );
      else if (srw && !getenv( "WASM_NO_NTDLL_SRW_FAST" ))
          fprintf( stderr, "wasm_x86: RtlReleaseSRWLockExclusive skeleton differs at %08x - left interpreted\n", srw );
    }
    { uint32_t wake = pe_export( base, "RtlWakeAddressAll" );
      /* RtlWakeAddressAll takes a per-address bucket lock before inspecting
       * the waiter list.  When that list is empty, the complete operation is
       * an observable no-op.  The packaged NTDLL export is pinned by the
       * loader/root artifact; retain its stable entry signature as the guard
       * and leave contended/non-empty buckets in Wine. */
      static const uint8_t head[] = { 0x55,0x89,0xe5,0x81,0xec,0x2c,
                                      0x04,0x00,0x00,0x89,0x7d,0xfc,
                                      0x8b,0x7d,0x08 };
      uint32_t b = wake + (uint32_t)nd_slide; int ok = wake != 0;
      for (unsigned i = 0; ok && i < 3; i++)
          if (rd8( b + i ) != head[i]) ok = 0;
      if (ok && !getenv( "WASM_NO_NTDLL_WAKE_FAST" )) {
          g_ntdll_wake_buckets = rd32( b + 17 );
          nat_register( wake, NAT_NTDLL_WAKE_ALL_EMPTY,
                        "ntdll RtlWakeAddressAll empty-bucket fast path" );
      } else if (wake && !getenv( "WASM_NO_NTDLL_WAKE_FAST" ))
          fprintf( stderr, "wasm_x86: RtlWakeAddressAll skeleton differs at %08x - left interpreted\n", wake );
    }
    if (getenv( "WASM_FAST_MATH" ))
        nat_register( pe_export( base, "floor" ), NAT_FLOOR, "floor (x87 return)" );
    base = find_module( c, "kernel32.dll" );
    { uint32_t tid = pe_export( base, "GetCurrentThreadId" );
      static const uint8_t code[] = { 0x64,0xa1,0x18,0x00,0x00,0x00,
                                      0x8b,0x40,0x24,0xc3 };
      uint32_t b = tid + (uint32_t)nd_slide; int ok = tid != 0;
      for (unsigned i = 0; ok && i < sizeof(code); i++)
          if (rd8( b + i ) != code[i]) ok = 0;
      if (ok) nat_register( tid, NAT_GETTID, "GetCurrentThreadId" );
      else if (tid) fprintf( stderr, "wasm_x86: GetCurrentThreadId skeleton differs at %08x - left interpreted\n", tid );
    }
    { uint32_t qpc = pe_export( base, "QueryPerformanceCounter" );
      uint32_t qpf = pe_export( base, "QueryPerformanceFrequency" );
      static const uint8_t thunk[] = { 0x8b,0xff,0x55,0x8b,0xec,0x5d,0xff,0x25 };
      uint32_t b = qpc + (uint32_t)nd_slide; int ok = qpc != 0;
      for (unsigned i = 0; ok && i < sizeof(thunk); i++)
          if (rd8( b + i ) != thunk[i]) ok = 0;
      if (ok) nat_register( qpc, NAT_QPC, "QueryPerformanceCounter" );
      else if (qpc) fprintf( stderr, "wasm_x86: QueryPerformanceCounter thunk differs at %08x - left interpreted\n", qpc );
      b = qpf + (uint32_t)nd_slide; ok = qpf != 0;
      for (unsigned i = 0; ok && i < sizeof(thunk); i++)
          if (rd8( b + i ) != thunk[i]) ok = 0;
      if (ok) nat_register( qpf, NAT_QPF, "QueryPerformanceFrequency" );
      else if (qpf) fprintf( stderr, "wasm_x86: QueryPerformanceFrequency thunk differs at %08x - left interpreted\n", qpf );
    }
    { uint32_t tls = pe_export( base, "TlsGetValue" );
      static const uint8_t thunk[] = { 0x8b,0xff,0x55,0x8b,0xec,0x5d,0xff,0x25 };
      uint32_t b = tls + (uint32_t)nd_slide; int ok = tls != 0;
      for (unsigned i = 0; ok && i < sizeof(thunk); i++)
          if (rd8( b + i ) != thunk[i]) ok = 0;
      if (ok) nat_register( tls, NAT_TLSGETVALUE, "TlsGetValue" );
      else if (tls) fprintf( stderr, "wasm_x86: TlsGetValue thunk differs at %08x - left interpreted\n", tls );
    }
    base = find_module( c, "kernelbase.dll" );
    { uint32_t tls = pe_export( base, "TlsGetValue" );
      static const uint8_t head[] = { 0x8b,0xff,0x55,0x8b,0xec };
      uint32_t b = tls + (uint32_t)nd_slide; int ok = tls != 0;
      for (unsigned i = 0; ok && i < sizeof(head); i++)
          if (rd8( b + i ) != head[i]) ok = 0;
      if (ok) nat_register( tls, NAT_TLSGETVALUE, "kernelbase TlsGetValue" );
      else if (tls) fprintf( stderr, "wasm_x86: kernelbase TlsGetValue skeleton differs at %08x - left interpreted\n", tls );
    }
    base = find_module( c, "ucrtbase.dll" );
    { uint32_t wc = pe_export( base, "wcschr" );
      static const uint8_t head[] = { 0x55,0x89,0xe5,0x8b,0x45,0x08,0x0f,0xb7,0x4d,0x0c };
      uint32_t b = wc + (uint32_t)nd_slide; int ok = wc != 0;
      for (unsigned i = 0; ok && i < sizeof(head); i++)
          if (rd8( b + i ) != head[i]) ok = 0;
      if (ok) nat_register( wc, NAT_WCSCHR, "ucrtbase wcschr" );
      else if (wc) fprintf( stderr, "wasm_x86: ucrtbase wcschr skeleton differs at %08x - left interpreted\n", wc );
    }
}

/* Run an intercepted CRT call natively.  cdecl: [esp]=return, args follow, the
 * caller cleans the stack, and the return value is the destination pointer.
 * Declines (returns 0, so the guest code runs as usual) if the buffers are not
 * wholly inside the guest address space. */
static void set_lazy( struct x86cpu *c, int kind, uint32_t op1, uint32_t op2, uint32_t res, int size );
static void run( struct x86cpu *c );

/* Audio bridge to the page (webwine/wasm_ipc.c). */
extern void webwine_audio_open( int freq, int channels );
extern int  webwine_audio_want( void );
extern int  webwine_audio_queued( void );
extern void webwine_audio_push( const void *buf, int frames, int channels, int fmt );

/* Call a cdecl function in the GUEST from C and return its EAX.
 *
 * The nested run() ends when the guest returns to GUEST_RET_SENTINEL, which is
 * below 0x10000 and so lands in wasm_x86_dispatch (the same route the syscall
 * trampolines use).  The whole CPU state is saved and restored, so whatever the
 * guest was doing resumes untouched; `stack_below` is the lowest guest address
 * the caller still needs, and the callee's frame is built under it. */
#define GUEST_RET_SENTINEL 0xfff0u
static int g_in_guest_call;
static int call_guest_cdecl( struct x86cpu *c, uint32_t func, int argc,
                             const uint32_t *argv, uint32_t stack_below )
{
    struct x86cpu saved = *c;
    uint32_t esp;
    int i;

    if (g_in_guest_call) return 0;               /* never re-enter */
    esp = (stack_below - 512 - (uint32_t)argc * 4) & ~15u;
    if (esp < 0x20000 || esp >= stack_below) return 0;
    /* Leave the callee real room below us: the guest's own stack limit is in its
     * TEB (NT_TIB: StackLimit at +0x08).  Refuse rather than run the mixer into
     * the guard page. */
    if (c->fs_base)
    {
        uint32_t limit = rd32( c->fs_base + 0x08 );
        if (limit && esp < limit + 0x40000) return 0;      /* keep 256K spare */
    }
    esp -= 4;
    wr32( esp, GUEST_RET_SENTINEL );
    for (i = 0; i < argc; i++) wr32( esp + 4 + i * 4, argv[i] );
    c->regs[ESP] = esp;
    c->eip = func;
    c->running = 1;
    g_in_guest_call = 1;
    run( c );
    g_in_guest_call = 0;
    *c = saved;
    return 1;
}

/* netduke32 walks its whole sprite-timer array once per frame inside
 * videoNextPage - 16,650 iterations of a five-instruction loop, ~10% of the
 * frame when interpreted one instruction at a time.  Run it natively.
 *
 * Like the ND_* frame-capture constants this build already depends on, the
 * address is specific to this executable, so it is NOT taken on trust: the exact
 * instruction bytes are verified before arming, and if they differ (another
 * build, another game) the loop is simply left to the interpreter. */
#define ND_AGELOOP 0x5196c0u
static const uint8_t nd_ageloop_code[] = {
    0xf6,0x40,0xfc,0x02,  /* testb $0x2,-0x4(%eax) */
    0x74,0x02,            /* je    +2              */
    0x01,0x18,            /* add   %ebx,(%eax)     */
    0x83,0xc0,0x40,       /* add   $0x40,%eax      */
    0x39,0xd0,            /* cmp   %edx,%eax       */
    0x75,0xf1             /* jne   loop            */
};

static void nat_arm_ageloop( void )
{
    uint32_t a = ND_AGELOOP + (uint32_t)nd_slide;
    unsigned i;

    for (i = 0; i < sizeof(nd_ageloop_code); i++)
        if (rd8( a + i ) != nd_ageloop_code[i]) break;
    if (i == sizeof(nd_ageloop_code)) nat_register( a, NAT_AGELOOP, "sprite-timer loop" );
    else fprintf( stderr, "wasm_x86: sprite-timer loop bytes differ at %08x - leaving it interpreted\n", a );
}

/* EXPERIMENT (env WASM_NAT_AGELOOP): run the engine's per-frame sprite-timer
 * walk natively to measure what it really costs.  Exact semantics of
 *   loop: testb $2,-4(%eax); je s; add %ebx,(%eax); s: add $0x40,%eax;
 *         cmp %edx,%eax; jne loop
 * which exits with eax==edx and ZF set. */
static int nat_ageloop( struct x86cpu *c )
{
    uint32_t p = c->regs[EAX], end = c->regs[EDX], inc = c->regs[EBX];
    const uint32_t GUEST_END = 0x70000000u;
    /* The guest loop is do-while and steps by 0x40, so only accelerate a range
     * that is ahead of p and 0x40-aligned; anything else falls back to the
     * interpreter rather than guessing. */
    if (p >= GUEST_END || end > GUEST_END || end <= p || ((end - p) & 0x3f)) return 0;
    do {
        if (rd8( p - 4 ) & 2) wr32( p, rd32( p ) + inc );
        p += 0x40;
    } while (p != end);
    c->regs[EAX] = p;
    set_lazy( c, K_SUB, p, end, 0, 4 );      /* the cmp that ends the loop */
    c->eip = ND_AGELOOP + (uint32_t)sizeof(nd_ageloop_code) + (uint32_t)nd_slide;
    return 1;
}

/* ---- cache1d::ageBlocks - the Build texture-cache LRU ager (guest 0x60b9c0) --
 * Walks cac[] (an array of 16-byte entries) decrementing the lock byte of every
 * ageable block (lock in [2,199]) and rotating the agecount cursor; blocks that
 * are free (lock 0/1) or locked (>=200, or 0xff) are skipped.  In the attract
 * demo this is ~5800 iterations/frame - the top UNHOOKED loop once the renderer
 * mappers run native - so run the whole function natively.  Its four globals are
 * absolute (relocated) addresses read straight from the code; the rest of the
 * 181-byte body is byte-verified so the fast path can only fire on the exact
 * routine.  WASM_NO_AGEBLOCKS disables it. */
#define ND_AGEBLOCKS 0x60b9c0u
static const uint8_t nd_ageblocks_code[181] = {
    0x55,0x89,0xe5,0x57,0x56,0x53,0x83,0xec,0x04,0xa1,0x74,0xc9,0xdf,0x00,0x8d,0x50,
    0xff,0x3b,0x05,0x94,0x9d,0xac,0x01,0x7f,0x06,0x89,0x15,0x94,0x9d,0xac,0x01,0xa1,
    0x70,0xc9,0xdf,0x00,0xc1,0xf8,0x04,0x39,0xd0,0x0f,0x4e,0xd0,0x89,0xd0,0x89,0x55,
    0xf0,0x8d,0x52,0xff,0x85,0xc0,0x74,0x75,0xa1,0x94,0x9d,0xac,0x01,0x8b,0x3d,0x60,
    0xc9,0xdf,0x00,0xeb,0x27,0x8d,0x76,0x00,0x8d,0x5e,0xff,0x88,0x19,0x83,0xe8,0x01,
    0x79,0x08,0xa1,0x74,0xc9,0xdf,0x00,0x83,0xe8,0x01,0x8d,0x4a,0xff,0x85,0xd2,0x74,
    0x47,0x8b,0x3d,0x60,0xc9,0xdf,0x00,0x89,0x55,0xf0,0x89,0xca,0x89,0xc1,0xc1,0xe1,
    0x04,0x8b,0x0c,0x0f,0x85,0xc9,0x74,0xd5,0x0f,0xb6,0x31,0x8d,0x5e,0xfe,0x0f,0xb6,
    0xdb,0x81,0xfb,0xc5,0x00,0x00,0x00,0x7e,0xbf,0x89,0xf3,0x80,0xfb,0xff,0x75,0xbd,
    0x83,0xe8,0x01,0x79,0x08,0xa1,0x74,0xc9,0xdf,0x00,0x83,0xe8,0x01,0x8b,0x55,0xf0,
    0x8d,0x4a,0xff,0xeb,0xc2,0x8d,0x76,0x00,0xa3,0x94,0x9d,0xac,0x01,0x83,0xc4,0x04,
    0x5b,0x5e,0x5f,0x5d,0xc3,
};
static const uint8_t nd_ageblocks_wild[181] = {
    0,0,0,0,0,0,0,0,0,0,1,1,1,1,0,0,
    0,0,0,1,1,1,1,0,0,0,0,1,1,1,1,0,
    1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,1,1,1,1,0,0,1,
    1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,1,1,1,1,0,0,0,0,0,0,0,0,0,
    0,0,0,1,1,1,1,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,1,1,1,1,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,1,1,1,1,0,0,0,
    0,0,0,0,0,
};

/* the four globals, resolved from the code at arm time (slid absolute addrs) */
static uint32_t nd_ab_cacnum, nd_ab_agecount, nd_ab_size, nd_ab_cac;

static void nat_arm_ageblocks( void )
{
    uint32_t a = ND_AGEBLOCKS + (uint32_t)nd_slide;
    unsigned i;
    for (i = 0; i < sizeof(nd_ageblocks_code); i++)
        if (!nd_ageblocks_wild[i] && rd8( a + i ) != nd_ageblocks_code[i]) break;
    if (i != sizeof(nd_ageblocks_code))
    { fprintf( stderr, "wasm_x86: ageBlocks bytes differ at %08x+%x - left interpreted\n", a, i ); return; }
    nd_ab_cacnum   = rd32( a + 0x0a );
    nd_ab_agecount = rd32( a + 0x13 );
    nd_ab_size     = rd32( a + 0x20 );
    nd_ab_cac      = rd32( a + 0x3f );
    /* each global is referenced more than once; require the repeats to agree so
     * a mis-slid read can never resolve half a pointer */
    if (rd32( a + 0x53 ) != nd_ab_cacnum || rd32( a + 0x96 ) != nd_ab_cacnum ||
        rd32( a + 0x1b ) != nd_ab_agecount || rd32( a + 0x39 ) != nd_ab_agecount ||
        rd32( a + 0xa9 ) != nd_ab_agecount || rd32( a + 0x63 ) != nd_ab_cac )
    { fprintf( stderr, "wasm_x86: ageBlocks globals inconsistent at %08x - left interpreted\n", a ); return; }
    nat_register( a, NAT_AGEBLOCKS, "cache1d::ageBlocks" );
}

static int nat_ageblocks( struct x86cpu *c )
{
    uint32_t cacnum   = rd32( nd_ab_cacnum );
    int32_t  agecount = (int32_t)rd32( nd_ab_agecount );
    uint32_t cac      = rd32( nd_ab_cac );
    int32_t  cnt = (int32_t)cacnum - 1;

    /* keep the cursor in range (guest: if cacnum <= agecount, agecount=cacnum-1) */
    if ((int32_t)cacnum <= agecount) { agecount = (int32_t)cacnum - 1; wr32( nd_ab_agecount, (uint32_t)agecount ); }
    int32_t sz = (int32_t)rd32( nd_ab_size ) >> 4;
    if (sz <= cnt) cnt = sz;                          /* cnt = min(cacnum-1, size/16) */

    if (cnt != 0)
    {
        int32_t cursor = agecount, remaining = cnt - 1;
        for (;;)
        {
            uint32_t ptr = rd32( cac + (uint32_t)cursor * 16u );
            if (ptr)
            {
                uint8_t lock = rd8( ptr );
                if ((uint8_t)(lock - 2) <= 0xc5) wr8( ptr, (uint8_t)(lock - 1) );  /* age it */
            }
            cursor -= 1;
            if (cursor < 0) cursor = (int32_t)cacnum - 1;      /* jns wrap */
            if (remaining == 0) break;
            remaining -= 1;
        }
        wr32( nd_ab_agecount, (uint32_t)cursor );
    }
    /* return: the routine preserves ebx/esi/edi/ebp (never touched here) and
     * trashes eax/ecx/edx (caller-saved); pop the return address off the stack. */
    c->eip = rd32( c->regs[ESP] );
    c->regs[ESP] += 4;
    return 1;
}

/* ---- qrhlineasm4 - the Build rotated horizontal texture mapper (guest 0x633310) -
 * Draws sloped floors/ceilings; the top per-frame guest loop once the other mappers
 * run native (~200k inner iterations/frame).  Same self-modifying family as
 * vlineasm4/mvlineasm4/mhlineskipmodify: the engine patches the two fixed-point steps
 * (sub/sbb into esi/ebx), the second-texel offset and the palookup base straight into
 * the 0x88888888 fields before each call, so read them from the code every call and
 * require the copies to agree.  Args arrive on the stack (cdecl): count then the
 * initial ebx/ecx/edx/esi/edi.  The routine saves and restores every register it uses,
 * so to the caller nothing but memory and eip changes.  WASM_NO_QRHLINE disables it. */
#define ND_SETUPQRHLINE 0x633290u
#define ND_QRHLINE 0x633310u

/* setupqrhlineasm4 is the SMC prologue immediately before qrhlineasm4.  The
 * mapper reads these values back from its instruction stream, so translating
 * only the body leaves stale steps behind and can strand the render loop. */
static int nat_setupqrhline( struct x86cpu *c )
{
    uint32_t sp = c->regs[ESP], base = ND_QRHLINE + (uint32_t)nd_slide;
    uint32_t arg0 = rd32( sp + 4 ), ebx = rd32( sp + 8 );
    uint32_t ecx = rd32( sp + 12 ), edx = rd32( sp + 16 );
    uint32_t doubled = ebx << 1, carry = ebx >> 31;
    uint32_t doubled_ecx = (ecx << 1) + carry;
    uint32_t neg_ecx = 0 - ecx;

    wr32( base + 0x45, ebx );       /* mov [qrhline+45], ebx */
    wr32( base + 0x4c, ecx );
    wr32( base + 0x72, neg_ecx );
    wr32( base + 0x98, neg_ecx );
    wr32( base + 0x78, doubled );
    wr32( base + 0x9e, doubled );
    wr32( base + 0x7e, doubled_ecx );
    wr32( base + 0xa4, doubled_ecx );
    wr32( base + 0x84, edx );
    wr32( base + 0x8a, edx );
    wr32( base + 0xaa, edx );
    wr32( base + 0xb0, edx );
    wr32( base + 0x52, edx );

    c->regs[EAX] = arg0;            /* the setup routine's final EAX value */
    set_lazy( c, K_ADC, ecx, ecx, doubled_ecx, 4 );
    c->lf_cin = (int)carry;
    c->eip = rd32( sp );            /* cdecl */
    c->regs[ESP] = sp + 4;
    return 1;
}

static void nat_arm_setupqrhline( void )
{
    static const uint8_t prefix[] = {
        0x53,0x51,0x52,0x56,0x57,
        0x8b,0x44,0x24,0x18, 0x8b,0x5c,0x24,0x1c,
        0x8b,0x4c,0x24,0x20, 0x8b,0x54,0x24,0x24,
        0x8b,0x74,0x24,0x28, 0x8b,0x7c,0x24,0x2c
    };
    uint32_t a = ND_SETUPQRHLINE + (uint32_t)nd_slide;
    unsigned i;
    for (i = 0; i < sizeof(prefix); i++) if (rd8( a + i ) != prefix[i]) break;
    if (i == sizeof(prefix) && rd8(a+0x1d)==0x89 && rd8(a+0x1e)==0x1d &&
        rd8(a+0x23)==0x89 && rd8(a+0x24)==0x0d && rd8(a+0x29)==0x31 &&
        rd8(a+0x2b)==0x29 && rd8(a+0x2d)==0x89 && rd8(a+0x39)==0x01 &&
        rd8(a+0x3b)==0x11)
        nat_register( a, NAT_SETUPQRHLINE, "setupqrhlineasm4" );
    else
        fprintf( stderr, "wasm_x86: setupqrhlineasm4 skeleton differs at %08x+%x - left interpreted\n", a, i );
}
static const uint8_t nd_qrhline_code[196] = {
    0x53,0x51,0x52,0x56,0x57,0x8b,0x44,0x24,0x18,0x8b,0x5c,0x24,0x1c,0x8b,0x4c,0x24,
    0x20,0x8b,0x54,0x24,0x24,0x8b,0x74,0x24,0x28,0x8b,0x7c,0x24,0x2c,0x55,0x83,0xf8,
    0x00,0x0f,0x8e,0x96,0x00,0x00,0x00,0x89,0xc5,0xf7,0xc5,0x03,0x00,0x00,0x00,0x74,
    0x33,0xeb,0x0d,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,
    0x8a,0x0b,0x4f,0x81,0xee,0x88,0x88,0x88,0x88,0x4d,0x81,0xdb,0x88,0x88,0x88,0x88,
    0x8a,0x81,0x88,0x88,0x88,0x88,0x88,0x07,0xf7,0xc5,0x03,0x00,0x00,0x00,0x75,0xe0,
    0x85,0xed,0x74,0x59,0x8a,0x0b,0xeb,0x08,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,
    0x8a,0x93,0x88,0x88,0x88,0x88,0x81,0xee,0x88,0x88,0x88,0x88,0x81,0xdb,0x88,0x88,
    0x88,0x88,0x8a,0xa1,0x88,0x88,0x88,0x88,0x8a,0x82,0x88,0x88,0x88,0x88,0x83,0xef,
    0x04,0xc1,0xe0,0x10,0x8a,0x0b,0x8a,0x93,0x88,0x88,0x88,0x88,0x81,0xee,0x88,0x88,
    0x88,0x88,0x81,0xdb,0x88,0x88,0x88,0x88,0x8a,0xa1,0x88,0x88,0x88,0x88,0x8a,0x82,
    0x88,0x88,0x88,0x88,0x8a,0x0b,0x89,0x07,0x83,0xed,0x04,0x75,0xb3,0x5d,0x5f,0x5e,
    0x5a,0x59,0x5b,0xc3,
};
static const uint8_t nd_qrhline_wild[196] = {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,1,1,1,1,0,0,0,1,1,1,1,
    0,0,1,1,1,1,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,1,1,1,1,0,0,1,1,1,1,0,0,1,1,
    1,1,0,0,1,1,1,1,0,0,1,1,1,1,0,0,
    0,0,0,0,0,0,0,0,1,1,1,1,0,0,1,1,
    1,1,0,0,1,1,1,1,0,0,1,1,1,1,0,0,
    1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,
};

/* The mapping itself.  Reads every patched immediate from its OWN site - the
 * engine does NOT patch them identically (the prologue steps 1 pixel, the main
 * loop 2 per sub-step, so their `sub $imm,%esi` values differ 2x), so replicate
 * each exactly where the guest uses it.  Writes only guest memory. */
static void qrhline_do( int32_t count, uint32_t ebx, uint32_t ecx, uint32_t edx,
                        uint32_t esi, uint32_t edi )
{
    uint32_t base = ND_QRHLINE + (uint32_t)nd_slide;
    uint32_t s_e = rd32(base+0x45), b_e = rd32(base+0x4c), pal_e = rd32(base+0x52);   /* prologue */
    uint32_t texb_a = rd32(base+0x72), s_a = rd32(base+0x78), b_a = rd32(base+0x7e);  /* main pair 1 */
    uint32_t pal_a = rd32(base+0x84), pal_b = rd32(base+0x8a);
    uint32_t texb_b = rd32(base+0x98), s_b = rd32(base+0x9e), b_b = rd32(base+0xa4);  /* main pair 2 */
    uint32_t pal_c = rd32(base+0xaa), pal_d = rd32(base+0xb0);

    if (count <= 0) return;
    int32_t ebp = count;
    /* 1-pixel prologue: run until the remaining count is a multiple of 4 */
    if (ebp & 3)
    {
        for (;;)
        {
            uint8_t cl = rd8( ebx );
            edi -= 1;
            uint32_t borrow = (esi < s_e);
            esi -= s_e;
            ebp -= 1;
            ebx = ebx - b_e - borrow;
            ecx = (ecx & 0xffffff00u) | cl;
            wr8( edi, rd8( ecx + pal_e ) );
            if (!(ebp & 3)) break;
        }
        if (ebp == 0) return;
    }
    /* 4-pixel main loop: two output pixels per sub-step (ebx and ebx+texb),
     * two sub-steps per iteration -> the four bytes p3..p0 written as a dword */
    uint8_t cl = rd8( ebx );
    for (;;)
    {
        uint32_t borrow;
        uint8_t dl = rd8( ebx + texb_a );
        borrow = (esi < s_a); esi -= s_a; ebx = ebx - b_a - borrow;
        ecx = (ecx & 0xffffff00u) | cl;
        edx = (edx & 0xffffff00u) | dl;
        uint8_t p3 = rd8( ecx + pal_a );    /* qrmach4a ecx -> ah */
        uint8_t p2 = rd8( edx + pal_b );    /* qrmach4b edx -> al */
        edi -= 4;
        cl = rd8( ebx );
        dl = rd8( ebx + texb_b );
        borrow = (esi < s_b); esi -= s_b; ebx = ebx - b_b - borrow;
        ecx = (ecx & 0xffffff00u) | cl;
        edx = (edx & 0xffffff00u) | dl;
        uint8_t p1 = rd8( ecx + pal_c );    /* qrmach4c ecx -> ah */
        uint8_t p0 = rd8( edx + pal_d );    /* qrmach4d edx -> al */
        cl = rd8( ebx );
        wr32( edi, ((uint32_t)p3 << 24) | ((uint32_t)p2 << 16) | ((uint32_t)p1 << 8) | p0 );
        ebp -= 4;
        if (ebp == 0) break;
    }
}

static int g_qr_filling;   /* recursion guard for the WASM_QRHLINE_VERIFY guest run */

static int nat_qrhline( struct x86cpu *c )
{
    if (g_qr_filling) return 0;                 /* nested verify run -> interpret */
    uint32_t esp = c->regs[ESP];
    int32_t  count = (int32_t)rd32( esp + 4 );
    uint32_t ebx = rd32( esp + 8 ), ecx = rd32( esp + 0xc ), edx = rd32( esp + 0x10 );
    uint32_t esi = rd32( esp + 0x14 ), edi = rd32( esp + 0x18 );

    static int verify = -1;
    if (verify < 0) verify = getenv( "WASM_QRHLINE_VERIFY" ) ? 1 : 0;
    if (verify && count > 0 && count <= 4096)
    {
        /* Differential check against the guest: the mapper only READS the
         * texture/palookup and WRITES the [edi-count, edi) span, so snapshot that
         * span, run native, stash the result, restore the span, then run the real
         * guest routine (guarded so it interprets) over the same args and compare.
         * The guest's output is what stays on screen. */
        static uint8_t snap[4096], nat[4096];
        uint32_t lo = edi - (uint32_t)count;
        int n = count, i;
        for (i = 0; i < n; i++) snap[i] = rd8( lo + i );
        qrhline_do( count, ebx, ecx, edx, esi, edi );
        for (i = 0; i < n; i++) nat[i] = rd8( lo + i );
        for (i = 0; i < n; i++) wr8( lo + i, snap[i] );
        uint32_t argv[6] = { (uint32_t)count, ebx, ecx, edx, esi, edi };
        g_qr_filling = 1;
        call_guest_cdecl( c, ND_QRHLINE + (uint32_t)nd_slide, 6, argv, esp );
        g_qr_filling = 0;
        for (i = 0; i < n; i++) if (nat[i] != rd8( lo + i ))
        {
            static int nbad = 0;
            if (nbad++ < 8)
                fprintf( stderr, "wasm_x86: QRHLINE MISMATCH count=%d off=%d native=%02x guest=%02x\n",
                         count, i, nat[i], rd8( lo + i ) );
            break;
        }
    }
    else qrhline_do( count, ebx, ecx, edx, esi, edi );

    c->eip = rd32( c->regs[ESP] );      /* cdecl: caller cleans the args */
    c->regs[ESP] += 4;
    return 1;
}

static void nat_arm_qrhline( void )
{
    uint32_t a = ND_QRHLINE + (uint32_t)nd_slide;
    unsigned i;
    for (i = 0; i < sizeof(nd_qrhline_code); i++)
        if (!nd_qrhline_wild[i] && rd8( a + i ) != nd_qrhline_code[i]) break;
    if (i == sizeof(nd_qrhline_code)) nat_register( a, NAT_QRHLINE, "qrhlineasm4" );
    else fprintf( stderr, "wasm_x86: qrhlineasm4 bytes differ at %08x+%x - left interpreted\n", a, i );
}

/* ---- SDL audio, driven by the interpreter -----------------------------------
 *
 * SDL_OpenAudioDevice starts an audio THREAD.  This interpreter has a single
 * guest CPU (one g_cpu), so that can never succeed, and every SDL audio backend
 * fails the same way regardless of SDL_AUDIODRIVER.  So we take SDL's place:
 * intercept its audio entry points, accept the device ourselves, and then call
 * the game's own audio callback from here, shipping the PCM to the page, which
 * plays it through Web Audio.
 *
 * Two things keep this honest:
 *  - the callback is only ever invoked when the game is NOT holding the SDL
 *    audio lock (we track SDL_LockAudioDevice depth), which is exactly the
 *    contract SDL itself provides for mixer state;
 *  - the entry points are netduke32's, so like the sprite-timer loop the exact
 *    thunk bytes are verified before arming and anything unexpected is left to
 *    the interpreter.
 */
#define SDL_A_OPEN        0x6a8140u   /* SDL_OpenAudio(desired, obtained)        */
#define SDL_A_OPENDEV     0x6a8170u   /* SDL_OpenAudioDevice(dev,cap,des,obt,ch) */
#define SDL_A_PAUSE       0x6a81a0u   /* SDL_PauseAudio(pause)                   */
#define SDL_A_PAUSEDEV    0x6a81b0u   /* SDL_PauseAudioDevice(dev, pause)        */
#define SDL_A_LOCK        0x6a8240u
#define SDL_A_LOCKDEV     0x6a8250u
#define SDL_A_UNLOCK      0x6a8260u
#define SDL_A_UNLOCKDEV   0x6a8270u
#define SDL_A_CLOSE       0x6a8280u   /* the one this game actually calls */
#define SDL_A_CLOSEDEV    0x6a8290u

/* every one of these is a 6-byte dynapi thunk: ff 25 <slot32> */
static int sdl_thunk_ok( uint32_t va, uint32_t slot )
{
    return rd8( va ) == 0xff && rd8( va + 1 ) == 0x25 && rd32( va + 2 ) == slot;
}

#define AUDIO_TARGET_FRAMES 6000   /* ~136ms at 44.1kHz */
static struct {
    int      armed, open, paused, lock;
    uint32_t callback, userdata;
    int      freq, channels, samples;
    uint32_t fmt;
    uint32_t size;          /* bytes the callback fills per call */
    uint64_t cb_insns, cb_calls;
    uint8_t  silence;
    uint64_t frames_out;
} g_aud;

static uint32_t garg( struct x86cpu *c, int i ) { return rd32( c->regs[ESP] + 4 + i * 4 ); }
static void gret( struct x86cpu *c, uint32_t eax )
{
    c->regs[EAX] = eax;
    c->eip = rd32( c->regs[ESP] );
    c->regs[ESP] += 4;
}

/* Accept an SDL audio device: remember the format + callback and report success. */
static int sdl_open_audio( struct x86cpu *c, int with_device )
{
    uint32_t desired = with_device ? garg( c, 2 ) : garg( c, 0 );
    uint32_t obtained = with_device ? garg( c, 3 ) : garg( c, 1 );
    int bits;

    if (!desired) { gret( c, with_device ? 0 : (uint32_t)-1 ); return 1; }

    g_aud.freq     = (int)rd32( desired + 0 );
    g_aud.fmt      = rd16( desired + 4 );
    g_aud.channels = rd8 ( desired + 6 );
    g_aud.samples  = rd16( desired + 8 );
    g_aud.callback = rd32( desired + 16 );
    g_aud.userdata = rd32( desired + 20 );

    if (g_aud.freq <= 0) g_aud.freq = 44100;
    if (g_aud.channels < 1 || g_aud.channels > 2) g_aud.channels = 2;
    if (g_aud.samples <= 0) g_aud.samples = 1024;
    bits = g_aud.fmt & 0xff;
    if (bits != 8 && bits != 16 && bits != 32) { g_aud.fmt = 0x8010; bits = 16; }
    g_aud.silence = (g_aud.fmt == 0x0008) ? 0x80 : 0x00;   /* AUDIO_U8 is centred at 128 */
    g_aud.size    = (uint32_t)g_aud.samples * g_aud.channels * (bits / 8);

    if (obtained)
    {
        wr32( obtained + 0, (uint32_t)g_aud.freq );
        wr16( obtained + 4, (uint16_t)g_aud.fmt );
        wr8 ( obtained + 6, (uint8_t)g_aud.channels );
        wr8 ( obtained + 7, g_aud.silence );
        wr16( obtained + 8, (uint16_t)g_aud.samples );
        wr16( obtained + 10, 0 );
        wr32( obtained + 12, g_aud.size );
        wr32( obtained + 16, g_aud.callback );
        wr32( obtained + 20, g_aud.userdata );
    }

    g_aud.open = 1;
    g_aud.paused = 1;            /* SDL opens paused */
    g_aud.lock = 0;
    webwine_audio_open( g_aud.freq, g_aud.channels );
    fprintf( stderr, "wasm_x86: SDL audio %dHz %dch fmt=%04x samples=%d cb=%08x (driven by us)\n",
             g_aud.freq, g_aud.channels, g_aud.fmt, g_aud.samples, g_aud.callback );
    gret( c, with_device ? 2u : 0u );   /* device id / success */
    return 1;
}

/* Call the game's audio callback and hand the PCM to the page.  The buffer lives
 * transiently below the guest stack pointer; the callback runs with its own frame
 * below that, and the interpreter state is restored afterwards. */
static void audio_pump( struct x86cpu *c )
{
    uint32_t saved_esp, buf, args[3];
    int want, rounds;

    if (!g_aud.open || g_aud.paused || g_aud.lock || !g_aud.callback) return;
  for (rounds = 0; rounds < 8; rounds++)
  {
    /* Keep only a small cushion queued (~140ms).  The callback is the game's own
     * mixer plus the OPL3 music synth running under the interpreter, so every
     * frame rendered ahead of time is real CPU taken from the renderer. */
    if (webwine_audio_queued() >= AUDIO_TARGET_FRAMES) break;
    want = webwine_audio_want();
    if (want < g_aud.samples) break;

    saved_esp = c->regs[ESP];
    buf = (saved_esp - 64 - g_aud.size) & ~15u;
    if (buf < 0x10000 || buf + g_aud.size > saved_esp) break;

    memset( (void *)(uintptr_t)buf, g_aud.silence, g_aud.size );
    args[0] = g_aud.userdata; args[1] = buf; args[2] = g_aud.size;
    {
        uint64_t before = g_total_insns;
        if (!call_guest_cdecl( c, g_aud.callback, 3, args, buf )) break;
        g_aud.cb_insns += g_total_insns - before;
        g_aud.cb_calls++;
    }

    webwine_audio_push( (const void *)(uintptr_t)buf, g_aud.samples, g_aud.channels, (int)g_aud.fmt );
    g_aud.frames_out += g_aud.samples;
  }
}

static void nat_arm_audio( void )
{
    static const struct { uint32_t va, slot; int kind; const char *name; } t[] = {
        { SDL_A_OPEN,      0x0082fde4u, NAT_SDL_OPEN,    "SDL_OpenAudio"        },
        { SDL_A_OPENDEV,   0x0082fdf0u, NAT_SDL_OPENDEV, "SDL_OpenAudioDevice"  },
        { SDL_A_PAUSE,     0x0082fdfcu, NAT_SDL_PAUSE,   "SDL_PauseAudio"       },
        { SDL_A_PAUSEDEV,  0x0082fe00u, NAT_SDL_PAUSEDEV,"SDL_PauseAudioDevice" },
        { SDL_A_LOCK,      0x0082fe1cu, NAT_SDL_LOCK,    "SDL_LockAudio"        },
        { SDL_A_LOCKDEV,   0x0082fe20u, NAT_SDL_LOCK,    "SDL_LockAudioDevice"  },
        { SDL_A_UNLOCK,    0x0082fe24u, NAT_SDL_UNLOCK,  "SDL_UnlockAudio"      },
        { SDL_A_UNLOCKDEV, 0x0082fe28u, NAT_SDL_UNLOCK,  "SDL_UnlockAudioDevice"},
        { SDL_A_CLOSE,     0x0082fe2cu, NAT_SDL_CLOSE,   "SDL_CloseAudio"       },
        { SDL_A_CLOSEDEV,  0x0082fe30u, NAT_SDL_CLOSE,   "SDL_CloseAudioDevice" },
    };
    unsigned i, ok = 0;

    for (i = 0; i < sizeof(t)/sizeof(t[0]); i++)
    {
        uint32_t va = t[i].va + (uint32_t)nd_slide;
        if (!sdl_thunk_ok( va, t[i].slot + (uint32_t)nd_slide )) continue;
        nat_register( va, t[i].kind, t[i].name );
        ok++;
    }
    if (ok != sizeof(t)/sizeof(t[0]))
        fprintf( stderr, "wasm_x86: SDL audio thunks: %u/%u matched; audio left to SDL\n",
                 ok, (unsigned)(sizeof(t)/sizeof(t[0])) );
    g_aud.armed = (ok == sizeof(t)/sizeof(t[0]));
}

/* ---- netduke32's 4-column texture mapper, run natively ----------------------
 *
 * `vlineasm4` (0x6321f3..0x63226a) is the Build engine's classic-renderer inner
 * loop: it walks four vertical spans at once, fetching a texel per column,
 * palettising it, packing four pixels into EAX and storing them as one dword.
 * A direct execution counter puts it at ~13,200 iterations per frame - about a
 * quarter of every guest instruction we interpret - which makes it the single
 * biggest remaining cost in the frame.
 *
 * It is also SELF-MODIFYING: the engine patches the shift counts, the fixed-point
 * step values and every texture/palette/framebuffer displacement directly into
 * the instruction stream before each call (they read as 0x88 filler on disk).
 * That is exactly why a block JIT cannot compile it once and reuse it - and why
 * running it natively here is easy by comparison: we simply read the current
 * immediates out of the code bytes on entry and interpret nothing.
 *
 * Safety: the OPCODE skeleton is verified byte-for-byte (only the patched
 * immediate fields are wildcards), so this cannot fire on some other code that
 * happens to live at this address; anything unexpected falls back to the
 * interpreter.  Memory is touched exactly as the guest instructions would.
 */
#define ND_VLINE 0x6321f3u
#define ND_VLINE_LEN 0x77u
#define ND_VLINE_DISPATCH 0x6320b3u

/* offset -> number of wildcard (patched) bytes; everything else must match */
static const struct { uint16_t off; uint8_t len; } nd_vline_imm[] = {
    { 0x02, 1 }, { 0x07, 4 }, { 0x0d, 4 }, { 0x13, 4 }, { 0x1a, 4 }, { 0x21, 4 },
    { 0x29, 1 }, { 0x2c, 4 }, { 0x32, 4 }, { 0x3d, 4 }, { 0x43, 1 }, { 0x47, 4 },
    { 0x4d, 1 }, { 0x51, 4 }, { 0x57, 4 }, { 0x5d, 4 }, { 0x63, 4 }, { 0x69, 4 },
    { 0x6f, 4 },
};
static const uint8_t nd_vline_code[ND_VLINE_LEN] = {
    0xc1,0xe9,0x00, 0x89,0xf3, 0x81,0xe3,0,0,0,0, 0x81,0xc2,0,0,0,0,
    0x81,0xd6,0,0,0,0, 0x0f,0xb6,0x89,0,0,0,0, 0x0f,0xb6,0x9b,0,0,0,0,
    0x89,0xe8, 0xc1,0xe8,0x00, 0x8a,0x89,0,0,0,0, 0x8a,0xab,0,0,0,0,
    0x89,0xeb, 0xc1,0xe1,0x10, 0x81,0xe3,0,0,0,0, 0x80,0xc2,0x00,
    0x0f,0xb6,0x80,0,0,0,0, 0x80,0xd6,0x00, 0x0f,0xb6,0x9b,0,0,0,0,
    0x81,0xd5,0,0,0,0, 0x8a,0x88,0,0,0,0, 0x8a,0xab,0,0,0,0,
    0x89,0x8f,0,0,0,0, 0x81,0xc7,0,0,0,0, 0x89,0xf1, 0x73,0x89
};

static int nd_vline_skeleton_ok( uint32_t va )
{
    uint8_t wild[ND_VLINE_LEN];
    unsigned i, k;

    memset( wild, 0, sizeof(wild) );
    for (i = 0; i < sizeof(nd_vline_imm)/sizeof(nd_vline_imm[0]); i++)
        for (k = 0; k < nd_vline_imm[i].len; k++) wild[nd_vline_imm[i].off + k] = 1;
    for (i = 0; i < ND_VLINE_LEN; i++)
        if (!wild[i] && rd8( va + i ) != nd_vline_code[i]) return 0;
    return 1;
}

/* The fixed prologue immediately before vlineasm4 is also hot.  It patches the
 * loop's immediates from the renderer globals and then jumps to ND_VLINE.  It
 * is safe to bypass only when both its entry bytes and its final jump still
 * have the expected shape; the absolute globals are deliberately read from
 * the guest memory rather than baked into this shortcut. */
static int nd_vline_dispatch_skeleton_ok( uint32_t va )
{
    static const uint8_t head[] = { 0x56,0x57,0x8b,0x4c,0x24,0x18,
                                    0x8b,0x7c,0x24,0x1c,0x55 };
    unsigned i;
    for (i = 0; i < sizeof(head); i++)
        if (rd8( va + i ) != head[i]) goto bad;
    if (rd8( va + 0x12a ) != 0x89 || rd8( va + 0x12b ) != 0xf1 ||
        rd8( va + 0x12c ) != 0xeb || rd8( va + 0x12d ) != 0x12) goto bad;
    return 1;
bad:
    if (getenv( "WASM_DUMP_VLINE_DISPATCH" ))
    {
        fprintf( stderr, "wasm_x86: vline dispatcher bytes:");
        for (i = 0; i < sizeof(head); i++) fprintf( stderr, " %02x", rd8( va + i ) );
        fprintf( stderr, " ...");
        for (i = 0; i < 9; i++) fprintf( stderr, " %02x", rd8( va + 0x12a + i ) );
        fprintf( stderr, "\n" );
    }
    return 0;
}

static int nat_vline_dispatch( struct x86cpu *c )
{
    uint32_t b = ND_VLINE + (uint32_t)nd_slide;
    uint32_t oldsp = c->regs[ESP], sp = oldsp - 8;
    uint32_t ecx, edi, eax, ebx, edx, esi, ebp, tab;

    /* The true dispatcher entry is three pushes before the verified setup
     * sequence.  If armed there, reproduce those pushes before sharing the
     * setup below.  The old one-instruction-late entry remains valid for a
     * previously generated table. */
    if (c->eip == ND_VLINE_DISPATCH + (uint32_t)nd_slide - 3u)
    {
        c->regs[ESP] -= 4; wr32( c->regs[ESP], c->regs[EBX] );
        c->regs[ESP] -= 4; wr32( c->regs[ESP], c->regs[ECX] );
        c->regs[ESP] -= 4; wr32( c->regs[ESP], c->regs[EDX] );
        oldsp = c->regs[ESP]; sp = oldsp - 8;
    }

    /* push %esi; push %edi; push %ebp */
    wr32( sp,     c->regs[EDI] );
    wr32( sp + 4, c->regs[ESI] );

    ecx = rd32( sp + 0x18 );
    edi = rd32( sp + 0x1c );
    wr32( sp - 4, c->regs[EBP] );
    c->regs[ESP] = sp - 4;
    tab = rd32( 0x0e168c4u + (uint32_t)nd_slide );
    eax = rd32( tab + ecx * 4 ) + edi;
    wr32( b + 0x69, eax );
    edi -= eax;

    /* The eight writes below are the call-specific immediates in the loop. */
    eax = rd32( 0x118b59cu + (uint32_t)nd_slide );
    ebx = rd32( 0x118b5a0u + (uint32_t)nd_slide );
    wr32( b + 0x1a, rd32( 0x118b5a4u + (uint32_t)nd_slide ) );
    wr32( b + 0x21, rd32( 0x118b5a8u + (uint32_t)nd_slide ) );
    wr32( b + 0x47, eax );
    wr32( b + 0x51, ebx );
    wr32( b + 0x2c, rd32( 0x118b5b8u + (uint32_t)nd_slide ) );
    wr32( b + 0x32, rd32( 0x118b5bcu + (uint32_t)nd_slide ) );
    wr32( b + 0x5d, rd32( 0x118b5b0u + (uint32_t)nd_slide ) );
    wr32( b + 0x63, rd32( 0x118b5b4u + (uint32_t)nd_slide ) );

    /* First pair: the x86 rol $0x88 is a rotate by eight bits. */
    ebp = rd32( 0x118b57cu + (uint32_t)nd_slide );
    ebx = rd32( 0x118b580u + (uint32_t)nd_slide );
    esi = rd32( 0x118b584u + (uint32_t)nd_slide );
    eax = rd32( 0x118b588u + (uint32_t)nd_slide );
    esi &= 0xfffffe00u; ebp &= 0xfffffe00u;
    eax = (eax << 8) | (eax >> 24); ebx = (ebx << 8) | (ebx >> 24);
    edx = (eax & 0xffff0000u) + (ebx >> 16);
    esi += eax & 0x1ffu; ebp += ebx & 0x1ffu;
    wr32( b + 0x0d, edx & 0xffff0000u );
    wr32( b + 0x13, esi );
    wr8 ( b + 0x43, (uint8_t)edx );
    wr8 ( b + 0x4d, (uint8_t)(edx >> 8) );
    wr32( b + 0x57, ebp );

    /* Second pair becomes the live register state at the loop entry. */
    ebp = rd32( 0x118b58cu + (uint32_t)nd_slide );
    ebx = rd32( 0x118b590u + (uint32_t)nd_slide );
    esi = rd32( 0x118b594u + (uint32_t)nd_slide );
    eax = rd32( 0x118b598u + (uint32_t)nd_slide );
    esi &= 0xfffffe00u; ebp &= 0xfffffe00u;
    eax = (eax << 8) | (eax >> 24); ebx = (ebx << 8) | (ebx >> 24);
    edx = (eax & 0xffff0000u) + (ebx >> 16);
    esi += eax & 0x1ffu; ebp += ebx & 0x1ffu;
    c->regs[EAX] = eax; c->regs[EBX] = ebx; c->regs[ECX] = esi;
    c->regs[EDX] = edx; c->regs[ESI] = esi; c->regs[EDI] = edi; c->regs[EBP] = ebp;
    c->eip = b;
    return 1;
}

static int nat_vlineasm4( struct x86cpu *c )
{
    uint32_t b = ND_VLINE + (uint32_t)nd_slide;
    /* current patched immediates - re-read every entry, since the engine rewrites
     * them for each span it draws */
    uint32_t sh1 = rd8 ( b + 0x02 ), sh2 = rd8 ( b + 0x29 );
    uint32_t and1 = rd32( b + 0x07 ), and2 = rd32( b + 0x3d );
    uint32_t stepd = rd32( b + 0x0d ), steps = rd32( b + 0x13 );
    uint32_t tex1 = rd32( b + 0x1a ), tex2 = rd32( b + 0x21 );
    uint32_t pal1 = rd32( b + 0x2c ), pal2 = rd32( b + 0x32 );
    uint32_t incdl = rd8 ( b + 0x43 ), incdh = rd8 ( b + 0x4d );
    uint32_t tex3 = rd32( b + 0x47 ), tex4 = rd32( b + 0x51 );
    uint32_t stepp = rd32( b + 0x57 );
    uint32_t pal3 = rd32( b + 0x5d ), pal4 = rd32( b + 0x63 );
    uint32_t fbdisp = rd32( b + 0x69 ), fbstep = rd32( b + 0x6f );
    uint32_t eax = c->regs[EAX], ebx = c->regs[EBX], ecx = c->regs[ECX];
    uint32_t edx = c->regs[EDX], esi = c->regs[ESI], edi = c->regs[EDI], ebp = c->regs[EBP];
    uint32_t cf = 0, prev_edi;
    uint64_t t;
    unsigned guard = 0;

    if (!fbstep) return 0;                 /* would never carry -> never terminate */
    /* A relocated/partially patched call can leave a very small framebuffer
     * step.  The original loop would then take millions of iterations before
     * its carry exit, which presents as a load percentage hang.  Normal spans
     * are a few hundred iterations; decline pathological calls before writing
     * anything so the interpreter handles them exactly. */
    if ((((uint64_t)0x100000000ULL - edi) + fbstep - 1) / fbstep > 65536)
        return 0;

    do {
        ecx >>= sh1;
        ebx = esi & and1;
        t = (uint64_t)edx + stepd;              edx = (uint32_t)t; cf = (uint32_t)(t >> 32);
        t = (uint64_t)esi + steps + cf;         esi = (uint32_t)t; cf = (uint32_t)(t >> 32);
        ecx = rd8( ecx + tex1 );
        ebx = rd8( ebx + tex2 );
        eax = ebp >> sh2;
        ecx = (ecx & 0xffffff00u) | rd8( ecx + pal1 );
        ecx = (ecx & 0xffff00ffu) | ((uint32_t)rd8( ebx + pal2 ) << 8);
        ebx = ebp & and2;
        ecx <<= 16;
        { uint32_t s = (edx & 0xff) + incdl;
          edx = (edx & 0xffffff00u) | (s & 0xff); cf = s >> 8; }
        eax = rd8( eax + tex3 );
        { uint32_t s = ((edx >> 8) & 0xff) + incdh + cf;
          edx = (edx & 0xffff00ffu) | ((s & 0xff) << 8); cf = s >> 8; }
        ebx = rd8( ebx + tex4 );
        t = (uint64_t)ebp + stepp + cf;         ebp = (uint32_t)t; cf = (uint32_t)(t >> 32);
        ecx = (ecx & 0xffffff00u) | rd8( eax + pal3 );
        ecx = (ecx & 0xffff00ffu) | ((uint32_t)rd8( ebx + pal4 ) << 8);
        wr32( edi + fbdisp, ecx );
        prev_edi = edi;
        t = (uint64_t)edi + fbstep;             edi = (uint32_t)t; cf = (uint32_t)(t >> 32);
        ecx = esi;
        guard++;
    } while (!cf && guard < (1u << 24));
    g_vl_calls++; g_vl_iters += guard;

    /* Fold the guest tail: publish the two fixed-point accumulators, then
     * synthesize the final add's flags and perform the six-register return.
     * The native entry is reached after the dispatcher's three initial pushes,
     * so this stack is exactly the layout consumed by the guest epilogue. */
    {
        uint32_t tail_eax, tail_edx, tail_esi, tail_ebp, sp = c->regs[ESP];
        wr32( 0x118b594u + (uint32_t)nd_slide, esi );
        wr32( 0x118b58cu + (uint32_t)nd_slide, ebp );
        tail_esi = esi << 8;
        tail_eax = edx;
        tail_ebp = ebp << 8;
        tail_edx = edx & 0xffffu;
        tail_eax >>= 8;
        tail_esi += tail_eax;
        tail_edx <<= 8;
        tail_ebp += tail_edx;
        wr32( 0x118b598u + (uint32_t)nd_slide, tail_esi );
        wr32( 0x118b590u + (uint32_t)nd_slide, tail_ebp );
        set_lazy( c, K_ADD, (ebp << 8), tail_edx, tail_ebp, 4 );
        c->regs[EAX] = tail_eax;
        c->regs[EBX] = rd32( sp + 20 );
        c->regs[ECX] = rd32( sp + 16 );
        c->regs[EDX] = rd32( sp + 12 );
        c->regs[ESI] = rd32( sp + 8 );
        c->regs[EDI] = rd32( sp + 4 );
        c->regs[EBP] = rd32( sp );
        c->regs[ESP] = sp + 24;
        c->eip = rd32( sp + 24 );
    }
    return 1;
}

static void nat_arm_vline( void )
{
    uint32_t va = ND_VLINE + (uint32_t)nd_slide;
    if (nd_vline_skeleton_ok( va )) nat_register( va, NAT_VLINE, "vlineasm4" );
    else fprintf( stderr, "wasm_x86: vlineasm4 skeleton differs at %08x - left interpreted\n", va );
}

static void nat_arm_vline_dispatch( void )
{
    uint32_t va = ND_VLINE_DISPATCH + (uint32_t)nd_slide;
    if (nd_vline_dispatch_skeleton_ok( va ))
        nat_register( va - 3u, NAT_VLINE_DISPATCH, "vlineasm4 dispatcher" );
    else fprintf( stderr, "wasm_x86: vlineasm4 dispatcher skeleton differs at %08x - left interpreted\n", va );
}

/* ---- mvlineasm4: the masked (transparent) 4-column texture mapper ----------
 *
 * The largest single cost left in gameplay - a sampled in-level profile puts
 * ~32% of ALL samples inside this one loop.  It is vlineasm4's masked sibling:
 * sprites, see-through walls and the player's weapon all go through it, so it
 * dominates exactly when vlineasm4 does not.
 *
 * Shape of Ken Silverman's asm (immediates patched per call by the C prologue):
 *
 *   beginmvlineasm4:   dec %cl / je endmvlineasm4          <- inner counter
 *   beginmvlineasm42:  for each of 4 columns: index = acc >> shift,
 *                      texel = tex[index], acc += step,
 *                      CF = (texel != 255), mask = mask*2 + CF,
 *                      pixel = shade[texel]
 *   fixchain2mb:       edi += bytesperline; jmp mvcase[mask]
 *   mvcase<mask>:      store only the opaque bytes; jmp beginmvlineasm4
 *   endmvlineasm4:     decb <counter> / jne beginmvlineasm42  <- outer counter
 *
 * The count is split across two 8-bit counters so the inner test can be a
 * one-byte `dec`, and two registers do double duty to free up a register:
 * %cl IS the low byte of %ecx's texture accumulator, and %dl IS the low byte of
 * %edx's.  That only works because the prologue zeroes the low byte of those
 * two steps (`xor %al,%al` before patching them in) and because the shifts
 * discard the low byte again.  We verify both rather than trust them, and fall
 * back to the interpreter if either fails - a wrong assumption here would
 * corrupt texture coordinates, which is far harder to spot than a crash.
 *
 * We run the whole two-level nest natively and re-enter the guest at
 * endmvlineasm4 with the outer counter still at its last value, so the guest
 * performs its own final `decb` - which means we never have to synthesise the
 * flags it leaves behind.
 */
#define ND_MVLINE     0x6325a0u
#define ND_MVLINE_LEN 0x90u
#define ND_MVCOUNT    0x00e18f88u    /* the outer counter (a shared scratch global) */
#define ND_MVLINE_DISPATCH 0x6324d5u

/* offset -> number of patched (wildcard) bytes; everything else must match */
static const struct { uint16_t off; uint8_t len; } nd_mvline_imm[] = {
    { 0x0e, 1 }, { 0x11, 1 }, { 0x14, 4 }, { 0x1a, 4 }, { 0x21, 4 }, { 0x28, 4 },
    { 0x37, 4 }, { 0x3d, 4 }, { 0x45, 1 }, { 0x4c, 4 }, { 0x56, 4 }, { 0x5c, 4 },
    { 0x64, 1 }, { 0x67, 4 }, { 0x6e, 4 }, { 0x78, 4 }, { 0x83, 4 }, { 0x8a, 4 },
};
static const uint8_t nd_mvline_code[ND_MVLINE_LEN] = {
    0xfe,0xc9,0x0f,0x84,0x88,0x00,0x00,0x00,0x89,0xe8,0x89,0xf3,
    0xc1,0xe8,0x20,0xc1,0xeb,0x20,0x81,0xc5,0x88,0x88,0x88,0x88,
    0x81,0xc6,0x88,0x88,0x88,0x88,0x0f,0xb6,0x80,0x88,0x88,0x88,
    0x88,0x0f,0xb6,0x9b,0x88,0x88,0x88,0x88,0x3c,0xff,0x10,0xd2,
    0x80,0xfb,0xff,0x10,0xd2,0x8a,0x9b,0x88,0x88,0x88,0x88,0x8a,
    0xb8,0x88,0x88,0x88,0x88,0x89,0xd0,0xc1,0xe8,0x20,0xc1,0xe3,
    0x10,0x0f,0xb6,0x80,0x88,0x88,0x88,0x88,0x3c,0xff,0x10,0xd2,
    0x81,0xc2,0x88,0x88,0x88,0x88,0x8a,0xb8,0x88,0x88,0x88,0x88,
    0x89,0xc8,0xc1,0xe8,0x20,0x81,0xc1,0x88,0x88,0x88,0x88,0x0f,
    0xb6,0x80,0x88,0x88,0x88,0x88,0x3c,0xff,0x10,0xd2,0x8a,0x98,
    0x88,0x88,0x88,0x88,0xc0,0xe2,0x04,0x31,0xc0,0x81,0xc7,0x40,
    0x01,0x00,0x00,0x88,0xd0,0x05,0x60,0x26,0x63,0x00,0xff,0xe0,
};
static const uint8_t nd_mvcase_code[256] = {
    0xe9,0x3b,0xff,0xff,0xff,0x90,0x90,0x90,0x90,0x90,0x90,0x90,
    0x90,0x90,0x90,0x90,0x88,0x1f,0xe9,0x29,0xff,0xff,0xff,0x90,
    0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x88,0x7f,0x01,0xe9,
    0x18,0xff,0xff,0xff,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,
    0x66,0x89,0x1f,0xe9,0x08,0xff,0xff,0xff,0x90,0x90,0x90,0x90,
    0x90,0x90,0x90,0x90,0xc1,0xeb,0x10,0x88,0x5f,0x02,0xe9,0xf5,
    0xfe,0xff,0xff,0x90,0x90,0x90,0x90,0x90,0x88,0x1f,0xc1,0xeb,
    0x10,0x88,0x5f,0x02,0xe9,0xe3,0xfe,0xff,0xff,0x90,0x90,0x90,
    0xc1,0xeb,0x08,0x66,0x89,0x5f,0x01,0xe9,0xd4,0xfe,0xff,0xff,
    0x90,0x90,0x90,0x90,0x66,0x89,0x1f,0xc1,0xeb,0x10,0x88,0x5f,
    0x02,0xe9,0xc2,0xfe,0xff,0xff,0x90,0x90,0xc1,0xeb,0x10,0x88,
    0x7f,0x03,0xe9,0xb5,0xfe,0xff,0xff,0x90,0x90,0x90,0x90,0x90,
    0x88,0x1f,0xc1,0xeb,0x10,0x88,0x7f,0x03,0xe9,0xa3,0xfe,0xff,
    0xff,0x90,0x90,0x90,0x88,0x7f,0x01,0xc1,0xeb,0x10,0x88,0x7f,
    0x03,0xe9,0x92,0xfe,0xff,0xff,0x90,0x90,0x66,0x89,0x1f,0xc1,
    0xeb,0x10,0x88,0x7f,0x03,0xe9,0x82,0xfe,0xff,0xff,0x90,0x90,
    0xc1,0xeb,0x10,0x66,0x89,0x5f,0x02,0xe9,0x74,0xfe,0xff,0xff,
    0x90,0x90,0x90,0x90,0x88,0x1f,0xc1,0xeb,0x10,0x66,0x89,0x5f,
    0x02,0xe9,0x62,0xfe,0xff,0xff,0x90,0x90,0x88,0x7f,0x01,0xc1,
    0xeb,0x10,0x66,0x89,0x5f,0x02,0xe9,0x51,0xfe,0xff,0xff,0x90,
    0x89,0x1f,0xe9,0x49,0xfe,0xff,0xff,0x90,0x90,0x90,0x90,0x90,
    0x90,0x90,0x90,0x90,
};

/* How each store case leaves %ebx: the cases that reach bytes 2/3 shift it down
 * first, and the caller can observe that, so reproduce it exactly. */
static const uint8_t nd_mvcase_shift[16] = {
    0, 0, 0, 0, 16, 16, 8, 16, 16, 16, 16, 16, 16, 16, 16, 0
};

static int nd_mvline_skeleton_ok( uint32_t va, uint32_t *casebase )
{
    uint8_t wild[ND_MVLINE_LEN];
    unsigned i, k;
    uint32_t cb;

    memset( wild, 0, sizeof(wild) );
    for (i = 0; i < sizeof(nd_mvline_imm)/sizeof(nd_mvline_imm[0]); i++)
        for (k = 0; k < nd_mvline_imm[i].len; k++) wild[nd_mvline_imm[i].off + k] = 1;
    for (i = 0; i < ND_MVLINE_LEN; i++)
        if (!wild[i] && rd8( va + i ) != nd_mvline_code[i]) return 0;
    /* the 16 store cases are ordinary code that is never patched, and their
     * jumps back are relative - so the whole table must match byte for byte */
    cb = rd32( va + 0x8a );
    /* the table lives a few dozen bytes past the loop; refuse anything else
     * rather than read 256 bytes through a pointer we decoded wrong */
    if (cb < va || cb - va > 0x400) return 0;
    for (i = 0; i < 256; i++)
        if (rd8( cb + i ) != nd_mvcase_code[i]) return 0;
    *casebase = cb;
    return 1;
}

static int nat_mvlineasm4( struct x86cpu *c )
{
    uint32_t b = ND_MVLINE + (uint32_t)nd_slide;
    uint32_t caller = rd32( c->regs[ESP] + 24 );
    uint32_t s16 = rd8 ( b + 0x0e ), s15 = rd8 ( b + 0x11 );
    uint32_t s14 = rd8 ( b + 0x45 ), s13 = rd8 ( b + 0x64 );
    uint32_t a12 = rd32( b + 0x14 ), a9 = rd32( b + 0x1a );
    uint32_t a6  = rd32( b + 0x56 ), a3 = rd32( b + 0x67 );
    const uint8_t *t3 = (const uint8_t *)(uintptr_t)rd32( b + 0x21 );   /* %ebp column */
    const uint8_t *t2 = (const uint8_t *)(uintptr_t)rd32( b + 0x28 );   /* %esi column */
    const uint8_t *t1 = (const uint8_t *)(uintptr_t)rd32( b + 0x4c );   /* %edx column */
    const uint8_t *t0 = (const uint8_t *)(uintptr_t)rd32( b + 0x6e );   /* %ecx column */
    const uint8_t *p3 = (const uint8_t *)(uintptr_t)rd32( b + 0x3d );
    const uint8_t *p2 = (const uint8_t *)(uintptr_t)rd32( b + 0x37 );
    const uint8_t *p1 = (const uint8_t *)(uintptr_t)rd32( b + 0x5c );
    const uint8_t *p0 = (const uint8_t *)(uintptr_t)rd32( b + 0x78 );
    uint32_t bpl = rd32( b + 0x83 ), cbase = rd32( b + 0x8a );
    uint32_t cnt = ND_MVCOUNT + (uint32_t)nd_slide;
    uint32_t eax = c->regs[EAX], ebx = c->regs[EBX], ecx = c->regs[ECX];
    uint32_t edx = c->regs[EDX], esi = c->regs[ESI], edi = c->regs[EDI], ebp = c->regs[EBP];
    uint32_t cl = ecx & 0xff, dl = edx & 0xff, mem = rd8( cnt );
    unsigned iters = 0;

    /* The masked mapper is also reached from relocatable compiled CON code.
     * Its SMC setup can move while a level is loading; do not let that caller
     * enter the native loop with a stale setup frame. */
    if (nd_dynamic_con_addr( caller )) return 0;

    /* the two shared registers only work if the steps leave the low byte alone
     * and the shifts throw it away again */
    if ((a3 & 0xff) || (a6 & 0xff)) return 0;
    if (s13 < 8 || s13 > 31 || s14 < 8 || s14 > 31 || s15 > 31 || s16 > 31) return 0;

    for (;;)
    {
        uint32_t x0, x1, x2, x3, mask;

        cl = (cl - 1) & 0xff;
        if (!cl)
        {
            if (((mem - 1) & 0xff) == 0) break;   /* leave the last dec to the guest */
            mem = (mem - 1) & 0xff;
        }
        x3 = t3[ ebp >> s16 ];  ebp += a12;
        x2 = t2[ esi >> s15 ];  esi += a9;
        x1 = t1[ edx >> s14 ];  edx += a6;
        x0 = t0[ ecx >> s13 ];  ecx += a3;
        mask = (uint32_t)(x3 != 0xff) << 3 | (uint32_t)(x2 != 0xff) << 2 |
               (uint32_t)(x1 != 0xff) << 1 | (uint32_t)(x0 != 0xff);
        ebx = (uint32_t)p0[x0] | (uint32_t)p1[x1] << 8 |
              (uint32_t)p2[x2] << 16 | (uint32_t)p3[x3] << 24;
        edi += bpl;
        if (mask)
        {
            uint8_t *d = (uint8_t *)(uintptr_t)edi;
            if (mask & 1) d[0] = (uint8_t)ebx;
            if (mask & 2) d[1] = (uint8_t)(ebx >> 8);
            if (mask & 4) d[2] = (uint8_t)(ebx >> 16);
            if (mask & 8) d[3] = (uint8_t)(ebx >> 24);
        }
        dl = (mask << 4) & 0xff;              /* what `shl $0x4,%dl` leaves */
        eax = cbase + dl;                     /* what the case dispatch leaves */
        ebx >>= nd_mvcase_shift[mask];        /* what the taken case leaves */
        iters++;
    }
    g_mv_calls++; g_mv_iters += iters;

    wr8( cnt, (uint8_t)mem );                 /* the guest's own decb takes it to 0 */
    c->regs[EAX] = eax; c->regs[EBX] = ebx;
    c->regs[ECX] = (ecx & 0xffffff00u) | cl;  /* %cl is spent */
    c->regs[EDX] = (edx & 0xffffff00u) | dl;
    c->regs[ESI] = esi; c->regs[EDI] = edi; c->regs[EBP] = ebp;
    c->eip = b + ND_MVLINE_LEN;               /* endmvlineasm4 */
    return 1;
}

static int nd_mvline_dispatch_skeleton_ok( uint32_t va )
{
    static const uint8_t head[] = { 0x8b,0x4c,0x24,0x18,0x8b,0x7c,0x24,0x1c,0x55 };
    static const uint8_t tail[] = { 0x81,0xef,0x40,0x01,0x00,0x00,0xeb,0x0c };
    unsigned i;
    for (i = 0; i < sizeof(head); i++) if (rd8( va + i ) != head[i]) return 0;
    for (i = 0; i < sizeof(tail); i++) if (rd8( va + 0xb7 + i ) != tail[i]) return 0;
    return 1;
}

/* Bypass mvlineasm4's self-modifying setup while preserving the six-register
 * frame expected by the existing native loop and its guest epilogue. */
static int nat_mvline_dispatch( struct x86cpu *c )
{
    uint32_t b = ND_MVLINE + (uint32_t)nd_slide;
    uint32_t entrysp, sp;
    uint32_t arg, edi, ebx, cl, mem;

    if (c->eip == ND_MVLINE_DISPATCH + (uint32_t)nd_slide - 5u &&
        nd_dynamic_con_addr( rd32( c->regs[ESP] ))) return 0;
    if (c->eip == ND_MVLINE_DISPATCH + (uint32_t)nd_slide - 4u &&
        nd_dynamic_con_addr( rd32( c->regs[ESP] + 4u ))) return 0;

    /* Calls enter at the true SMC preamble, which pushes five registers before
     * reaching the old hook at ND_MVLINE_DISPATCH.  When this hook is armed at
     * the true entry, reproduce all five pushes.  Keep the one-instruction-late
     * case as a compatibility guard for an already-installed older table. */
    if (c->eip == ND_MVLINE_DISPATCH + (uint32_t)nd_slide - 5u)
    {
        c->regs[ESP] -= 4; wr32( c->regs[ESP], c->regs[EBX] );
        c->regs[ESP] -= 4; wr32( c->regs[ESP], c->regs[ECX] );
        c->regs[ESP] -= 4; wr32( c->regs[ESP], c->regs[EDX] );
        c->regs[ESP] -= 4; wr32( c->regs[ESP], c->regs[ESI] );
        c->regs[ESP] -= 4; wr32( c->regs[ESP], c->regs[EDI] );
    }
    else if (c->eip == ND_MVLINE_DISPATCH + (uint32_t)nd_slide - 4u)
    {
        c->regs[ESP] -= 4; wr32( c->regs[ESP], c->regs[ECX] );
        c->regs[ESP] -= 4; wr32( c->regs[ESP], c->regs[EDX] );
        c->regs[ESP] -= 4; wr32( c->regs[ESP], c->regs[ESI] );
        c->regs[ESP] -= 4; wr32( c->regs[ESP], c->regs[EDI] );
    }
    entrysp = c->regs[ESP];
    sp = entrysp - 4;

    /* The first five pushes (ebx, ecx, edx, esi, edi) precede the registered
     * address.  This address performs the final push ebp. */
    wr32( sp, c->regs[EBP] );
    c->regs[ESP] = sp;
    arg = rd32( entrysp + 0x18 );
    edi = rd32( entrysp + 0x1c );

    wr32( b + 0x6e, rd32( 0x118b59cu + (uint32_t)nd_slide ) );
    wr32( b + 0x4c, rd32( 0x118b5a0u + (uint32_t)nd_slide ) );
    wr32( b + 0x28, rd32( 0x118b5a4u + (uint32_t)nd_slide ) );
    wr32( b + 0x21, rd32( 0x118b5a8u + (uint32_t)nd_slide ) );
    wr32( b + 0x78, rd32( 0x118b5b0u + (uint32_t)nd_slide ) );
    wr32( b + 0x5c, rd32( 0x118b5b4u + (uint32_t)nd_slide ) );
    wr32( b + 0x37, rd32( 0x118b5b8u + (uint32_t)nd_slide ) );
    wr32( b + 0x3d, rd32( 0x118b5bcu + (uint32_t)nd_slide ) );
    wr32( b + 0x67, rd32( 0x118b57cu + (uint32_t)nd_slide ) & 0xffffff00u );
    wr32( b + 0x56, rd32( 0x118b580u + (uint32_t)nd_slide ) & 0xffffff00u );
    wr32( b + 0x1a, rd32( 0x118b584u + (uint32_t)nd_slide ) );
    wr32( b + 0x14, rd32( 0x118b588u + (uint32_t)nd_slide ) );

    ebx = arg;
    c->regs[ECX] = rd32( 0x118b58cu + (uint32_t)nd_slide );
    c->regs[EDX] = rd32( 0x118b590u + (uint32_t)nd_slide );
    c->regs[ESI] = rd32( 0x118b594u + (uint32_t)nd_slide );
    c->regs[EBP] = rd32( 0x118b598u + (uint32_t)nd_slide );
    cl = ((c->regs[ECX] & 0xffffff00u) | (ebx & 0xffu)) + 1u;
    ebx += 0x00000100u;
    mem = (ebx >> 8) & 0xffu;
    wr8( ND_MVCOUNT + (uint32_t)nd_slide, (uint8_t)mem );
    c->regs[ECX] = (cl & 0xffu) | (c->regs[ECX] & 0xffffff00u);
    c->regs[EBX] = ebx;
    c->regs[EDI] = edi - 0x140u;
    c->eip = b;
    return 1;
}

static void nat_arm_mvline( void )
{
    uint32_t va = ND_MVLINE + (uint32_t)nd_slide, cb = 0;
    if (nd_mvline_skeleton_ok( va, &cb )) nat_register( va, NAT_MVLINE, "mvlineasm4" );
    else fprintf( stderr, "wasm_x86: mvlineasm4 skeleton differs at %08x - left interpreted\n", va );
}

static void nat_arm_mvline_dispatch( void )
{
    uint32_t va = ND_MVLINE_DISPATCH + (uint32_t)nd_slide;
    if (nd_mvline_dispatch_skeleton_ok( va ))
        nat_register( va - 5u, NAT_MVLINE_DISPATCH, "mvlineasm4 dispatcher" );
    else fprintf( stderr, "wasm_x86: mvlineasm4 dispatcher skeleton differs at %08x - left interpreted\n", va );
}

/* ---- 8-bpp -> 32-bpp surface blit, run natively -----------------------------
 *
 * videoNextPage's inner conversion loop (0x519f87): read a 16-bit pair from the
 * 8-bpp frame, map it through a byte table, look the result up in a 32-bit
 * palette and store one output pixel.  Seven instructions per pixel over the
 * whole frame, which after vlineasm4 went native is the biggest thing left.
 *
 * Note this work is pure overhead FOR US - we present by reading `frameplace`
 * (the 8-bpp buffer) and palettising it ourselves - but the game still owes SDL
 * a converted surface, so rather than skip its output we just stop interpreting
 * it.  Same self-validating pattern as vlineasm4: the only non-fixed field is
 * the absolute palette address, which is read from the instruction. */
#define ND_SURFBLIT 0x519f87u
#define ND_SURFBLIT_LEN 27u
static const uint8_t nd_surfblit_code[ND_SURFBLIT_LEN] = {
    0x0f,0xb7,0x16,             /* movzwl (%esi),%edx            */
    0x83,0xc0,0x04,             /* add    $0x4,%eax              */
    0x83,0xc6,0x02,             /* add    $0x2,%esi              */
    0x0f,0xb6,0x14,0x17,        /* movzbl (%edi,%edx,1),%edx     */
    0x8b,0x14,0x95,0,0,0,0,     /* mov    PAL(,%edx,4),%edx      */
    0x89,0x50,0xfc,             /* mov    %edx,-0x4(%eax)        */
    0x39,0xd8,                  /* cmp    %ebx,%eax              */
    0x72,0xe5                   /* jb     loop                   */
};

static int nat_surfblit( struct x86cpu *c )
{
    uint32_t b = ND_SURFBLIT + (uint32_t)nd_slide;
    uint32_t pal = rd32( b + 16 );                 /* absolute palette base */
    uint32_t eax = c->regs[EAX], ebx = c->regs[EBX];
    uint32_t esi = c->regs[ESI], edi = c->regs[EDI], edx;
    unsigned guard = 0;

    do {
        edx = rd16( esi );
        eax += 4;
        esi += 2;
        edx = rd8( edi + edx );
        edx = rd32( pal + edx * 4 );
        wr32( eax - 4, edx );
        guard++;
    } while (eax < ebx && guard < (1u << 24));
    g_sb_calls++; g_sb_iters += guard;

    c->regs[EAX] = eax; c->regs[ESI] = esi; c->regs[EDX] = edx;
    set_lazy( c, K_SUB, eax, ebx, eax - ebx, 4 );  /* the cmp that ended it */
    c->eip = b + ND_SURFBLIT_LEN;
    return 1;
}

static void nat_arm_surfblit( void )
{
    uint32_t va = ND_SURFBLIT + (uint32_t)nd_slide;
    unsigned i;
    for (i = 0; i < ND_SURFBLIT_LEN; i++)
    {
        if (i >= 16 && i < 20) continue;           /* palette address: wildcard */
        if (rd8( va + i ) != nd_surfblit_code[i] )
        {
            fprintf( stderr, "wasm_x86: surface-blit skeleton differs at %08x - left interpreted\n", va );
            return;
        }
    }
    nat_register( va, NAT_SURFBLIT, "surface blit" );
}

/* ---- mouse capture (pointer lock) ------------------------------------------
 *
 * The game never polls mouse state (SDL_GetRelativeMouseState has zero call
 * sites); it reads SDL_MOUSEMOTION events and uses their xrel/yrel, after asking
 * for SDL_SetRelativeMouseMode.  Real SDL answers that by switching its Windows
 * backend to RAW INPUT (WM_INPUT), which we do not deliver - so relative mouse
 * would produce nothing no matter what WM_MOUSEMOVEs we post.
 *
 * So: accept SetRelativeMouseMode (the game hides its cursor and starts using
 * xrel/yrel), and synthesise the motion events ourselves from the pointer-lock
 * deltas the page collects.  SDL_PollEvent is intercepted only while a delta is
 * pending; otherwise it declines and the real SDL_PollEvent runs, so the normal
 * event stream (keys, buttons, quit) is untouched.
 *
 * Layout below is from the exe's own DWARF: SDL_MouseMotionEvent is 36 bytes,
 * type@0 timestamp@4 windowID@8 which@12 state@16 x@20 y@24 xrel@28 yrel@32,
 * and SDL_MOUSEMOTION == 1024. */
#define SDL_A_POLL      0x6a8420u
#define SDL_A_RELMOUSE  0x6a8c80u
#define SDL_A_KEYBOARDSTATE 0x6a8a80u
#define SDL_EV_MOUSEMOTION 1024u

/* Filled by the win32u ring drain (dlls/win32u/message.c) from page events. */
int g_mouse_dx, g_mouse_dy;      /* accumulated pointer-lock motion */
int g_mouse_buttons;             /* live SDL button mask (win32u ring drain) */
int g_mouse_x = 160, g_mouse_y = 100;
int g_wasm_sdl_poll_fallback;    /* short window in which real SDL must drain input */
extern void wasm_drain_browser_input(void);

/* Browser input is drained by win32u while SDL is being polled.  The native
 * SDL_PollEvent hook cannot call back into the original Windows SDL thunk, so
 * retain the posted events here and expose them as real SDL event records on
 * the next poll.  This also avoids relying on the null Wine user driver to
 * route a message to the SDL window. */
#define WASM_INPUT_Q 128
struct wasm_key_input { int vk, scancode, up; };
static struct wasm_key_input g_wasm_key_q[WASM_INPUT_Q];
static unsigned g_wasm_key_head, g_wasm_key_tail;
struct wasm_mouse_input { int type, a, b, c; };
static struct wasm_mouse_input g_wasm_mouse_q[WASM_INPUT_Q];
static unsigned g_wasm_mouse_head, g_wasm_mouse_tail;
void wasm_queue_key_input( int vk, int scancode, int up )
{
    unsigned next = (g_wasm_key_head + 1) % WASM_INPUT_Q;
    if (next == g_wasm_key_tail) return;
    g_wasm_key_q[g_wasm_key_head] = (struct wasm_key_input){ vk, scancode, up };
    g_wasm_key_head = next;
}
void wasm_queue_mouse_input( int type, int a, int b, int c )
{
    unsigned next = (g_wasm_mouse_head + 1) % WASM_INPUT_Q;
    if (next == g_wasm_mouse_tail) return;
    g_wasm_mouse_q[g_wasm_mouse_head] = (struct wasm_mouse_input){ type, a, b, c };
    g_wasm_mouse_head = next;
}
static int g_rel_mouse_on;       /* game asked for SDL relative (aiming) mode */
static int g_mb_reported;        /* button mask the game has already been given */
static uint8_t g_sdl_keyboard[512];

static int sdl_keyboard_state( struct x86cpu *c )
{
    uint32_t n = garg( c, 0 );
    if (n) wr32( n, 512 );
    gret( c, (uint32_t)(uintptr_t)g_sdl_keyboard );
    return 1;
}
static unsigned g_wasm_input_trace;

/* SDL's keysym.sym is not a Windows VK.  Printable letters use lowercase
 * ASCII, while navigation/function keys use SDLK_SCANCODE_MASK|scancode. */
static uint32_t wasm_sdl_keycode( int vk, int scancode )
{
    if (vk >= 'A' && vk <= 'Z') return (uint32_t)(vk + ('a' - 'A'));
    if (vk >= '0' && vk <= '9') return (uint32_t)vk;
    switch (vk)
    {
    case 0x08: return 8;       /* backspace */
    case 0x09: return 9;       /* tab */
    case 0x0d: return 13;      /* return */
    case 0x1b: return 27;      /* escape */
    case 0x20: return 32;      /* space */
    case 0x25: return 0x40000050; /* left */
    case 0x26: return 0x40000052; /* up */
    case 0x27: return 0x4000004f; /* right */
    case 0x28: return 0x40000051; /* down */
    case 0x70: case 0x71: case 0x72: case 0x73:
    case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7a: case 0x7b:
        return 0x40000000u | (uint32_t)(scancode ? scancode : (vk - 0x70 + 58));
    default:
        return scancode ? (0x40000000u | (uint32_t)scancode) : (uint32_t)vk;
    }
}

/* SDL_MouseButtonEvent (from the exe's DWARF, 28 bytes): type@0 timestamp@4
 * windowID@8 which@12 button@16(u8) state@17(u8) clicks@18(u8) pad@19 x@20 y@24. */
#define SDL_EV_MOUSEBUTTONDOWN 1025u
#define SDL_EV_MOUSEBUTTONUP   1026u
#define SDL_EV_KEYDOWN         768u
#define SDL_EV_KEYUP           769u

static int wasm_sdl_queued_event( struct x86cpu *c, uint32_t ev )
{
    if (g_wasm_key_tail != g_wasm_key_head)
    {
        struct wasm_key_input in = g_wasm_key_q[g_wasm_key_tail];
        g_wasm_key_tail = (g_wasm_key_tail + 1) % WASM_INPUT_Q;
        wr32( ev + 0, in.up ? SDL_EV_KEYUP : SDL_EV_KEYDOWN );
        wr32( ev + 4, 0 );                 /* timestamp */
        wr32( ev + 8, 1 );                 /* windowID */
        wr8 ( ev + 12, in.up ? 0 : 1 );    /* state */
        wr8 ( ev + 13, 0 );                /* repeat */
        wr32( ev + 16, (uint32_t)in.scancode );
        wr32( ev + 20, wasm_sdl_keycode( in.vk, in.scancode ) );
        wr32( ev + 24, 0 );                /* modifier */
        wr32( ev + 28, 0 );
        if (in.scancode < sizeof(g_sdl_keyboard)) g_sdl_keyboard[in.scancode] = !in.up;
        if (g_wasm_input_trace < 16)
        {
            fprintf( stderr, "wasm_input: SDL key %s vk=%#x scan=%#x sym=%#x\\n",
                     in.up ? "up" : "down", in.vk, in.scancode,
                     wasm_sdl_keycode( in.vk, in.scancode ) );
            g_wasm_input_trace++;
        }
        gret( c, 1 );
        return 1;
    }
    if (g_wasm_mouse_tail != g_wasm_mouse_head)
    {
        struct wasm_mouse_input in = g_wasm_mouse_q[g_wasm_mouse_tail];
        g_wasm_mouse_tail = (g_wasm_mouse_tail + 1) % WASM_INPUT_Q;
        if (in.type == 3)
        {
            wr32( ev + 0, SDL_EV_MOUSEMOTION ); wr32( ev + 4, 0 );
            wr32( ev + 8, 1 ); wr32( ev + 12, 0 );
            wr32( ev + 16, (uint32_t)g_mouse_buttons );
            wr32( ev + 20, (uint32_t)in.a ); wr32( ev + 24, (uint32_t)in.b );
            wr32( ev + 28, 0 ); wr32( ev + 32, 0 );
        }
        else
        {
            int button = in.a == 2 ? 3 : in.a == 1 ? 2 : 1;
            wr32( ev + 0, in.type == 4 ? SDL_EV_MOUSEBUTTONDOWN : SDL_EV_MOUSEBUTTONUP );
            wr32( ev + 4, 0 ); wr32( ev + 8, 1 ); wr32( ev + 12, 0 );
            wr8 ( ev + 16, (uint8_t)button ); wr8( ev + 17, in.type == 4 );
            wr8 ( ev + 18, 1 ); wr8( ev + 19, 0 );
            wr32( ev + 20, (uint32_t)in.b ); wr32( ev + 24, (uint32_t)in.c );
        }
        gret( c, 1 );
        return 1;
    }
    return 0;
}

static int sdl_poll_event( struct x86cpu *c )
{
    static uint64_t last_synth_flip;
    static int fast_empty = -1;
    uint32_t ev = garg( c, 0 );
    int dx, dy;

    if (!ev) return 0;
    /* SDL games often poll without entering the Win32 message pump.  Drain
     * the browser ring here so input is available on the same poll that
     * consumes it, rather than depending on an unrelated PeekMessage call. */
    wasm_drain_browser_input();
    if (fast_empty < 0) fast_empty = getenv( "WASM_NO_FAST_EMPTY_POLL" ) ? 0 : 1;

    /* In the browser, win32u marks polls that may have a newly posted input
     * message so the real SDL queue gets a chance to drain it.  Outside that
     * short window there cannot be an OS event source: all browser input enters
     * through the same ring, and relative mouse/buttons are handled below.
     * Returning zero here avoids re-entering the large SDL thunk on every empty
     * gameplay poll.  The opt-out is useful as a differential control. */
    if (!g_wasm_sdl_poll_fallback && fast_empty &&
        (!g_rel_mouse_on || (!g_mouse_dx && !g_mouse_dy &&
                             g_mouse_buttons == g_mb_reported)))
    {
        gret( c, 0 );
        return 1;
    }
    if (g_wasm_sdl_poll_fallback) g_wasm_sdl_poll_fallback--;

    if (wasm_sdl_queued_event( c, ev )) return 1;

    /* Emit one pending mouse-button transition per poll (a click is two: down
     * then up, so the game's drain loop still empties).  Gated on relative mode:
     * in the menus the cursor is absolute and SDL delivers the real WM_*BUTTON
     * events, so leave those alone and avoid double-clicks.  g_mouse_buttons:
     * bit1=left, bit2=middle, bit4=right (win32u ring drain). */
    if (g_rel_mouse_on)
    {
        int diff = g_mouse_buttons ^ g_mb_reported;
        if (diff)
        {
            int lowbit = diff & -diff;                       /* one button at a time */
            int down   = (g_mouse_buttons & lowbit) != 0;
            int sdlbtn = lowbit == 1 ? 1 : lowbit == 2 ? 2 : 3;  /* L / M / R */
            g_mb_reported ^= lowbit;
            wr32( ev + 0,  down ? SDL_EV_MOUSEBUTTONDOWN : SDL_EV_MOUSEBUTTONUP );
            wr32( ev + 4,  0 );                              /* timestamp */
            wr32( ev + 8,  1 );                              /* windowID */
            wr32( ev + 12, 0 );                              /* which */
            wr8 ( ev + 16, (uint8_t)sdlbtn );
            wr8 ( ev + 17, (uint8_t)(down ? 1 : 0) );        /* SDL_PRESSED / RELEASED */
            wr8 ( ev + 18, 1 );                              /* clicks */
            wr8 ( ev + 19, 0 );
            wr32( ev + 20, (uint32_t)g_mouse_x );
            wr32( ev + 24, (uint32_t)g_mouse_y );
            gret( c, 1 );
            return 1;
        }
    }

    dx = g_mouse_dx; dy = g_mouse_dy;
    if (!dx && !dy) return 0;                /* nothing pending: run the real one */

    /* At most one synthesised motion event per rendered frame.  The win32u ring
     * drain refills g_mouse_dx/dy from inside the game's own PeekMessage pump,
     * which runs inside its "while (SDL_PollEvent(&e))" drain loop - so if the
     * pointer keeps moving while a frame is in flight (a level load pumps events
     * without flipping), synthesising on every poll makes that loop never see an
     * empty queue and it spins forever, wedging the load.  Real SDL snapshots the
     * OS queue once per pump; coalescing to one event per flip is the same shape:
     * the game still gets the whole delta each frame for smooth mouselook, and a
     * flipless load drains empty after the first event. */
    if (g_flip_count == last_synth_flip) return 0;
    last_synth_flip = g_flip_count;

    g_mouse_dx = 0; g_mouse_dy = 0;
    g_mouse_x += dx; g_mouse_y += dy;

    wr32( ev + 0,  SDL_EV_MOUSEMOTION );
    wr32( ev + 4,  0 );                      /* timestamp: unused by the game */
    wr32( ev + 8,  1 );                      /* windowID: its only window */
    wr32( ev + 12, 0 );                      /* which: mouse index */
    wr32( ev + 16, (uint32_t)g_mouse_buttons );
    wr32( ev + 20, (uint32_t)g_mouse_x );
    wr32( ev + 24, (uint32_t)g_mouse_y );
    wr32( ev + 28, (uint32_t)dx );
    wr32( ev + 32, (uint32_t)dy );
    gret( c, 1 );                            /* an event is available */
    return 1;
}

static void nat_arm_mouse( void )
{
    static const struct { uint32_t va, slot; int kind; const char *name; } t[] = {
        { SDL_A_POLL,     0x0082fe94u, NAT_SDL_POLL,     "SDL_PollEvent"           },
        { SDL_A_RELMOUSE, 0x00830090u, NAT_SDL_RELMOUSE, "SDL_SetRelativeMouseMode" },
        { SDL_A_KEYBOARDSTATE, 0x00830010u, NAT_SDL_KEYBOARDSTATE, "SDL_GetKeyboardState" },
    };
    unsigned i;
    for (i = 0; i < sizeof(t)/sizeof(t[0]); i++)
    {
        uint32_t va = t[i].va + (uint32_t)nd_slide;
        if (sdl_thunk_ok( va, t[i].slot + (uint32_t)nd_slide ))
            nat_register( va, t[i].kind, t[i].name );
        else
            fprintf( stderr, "wasm_x86: %s thunk differs - mouse capture off\n", t[i].name );
    }
}

/* ---- the WHOLE 8-bpp -> 32-bpp span, not just its remainder loop -----------
 *
 * The conversion above turned out to be only the tail.  The compiler also
 * emitted a 64-pixel unrolled copy of the same three lookups, and that is where
 * 80% of the pixels actually go - the tail was doing 12800 of 64000 a frame.
 *
 * It hid for a simple reason: a fresh in-level profile put 34% of everything in
 * _videoNextPage but spread it FLAT over a 1255-byte range, ~0.14% per address,
 * because straight-line unrolled code has no hot address to point at.  A top-N
 * address profile will never show that; only summing per function does.
 *
 * Rather than intercept the unrolled block alone we take the whole span -
 * guard, unrolled block, fixup arithmetic and tail loop - and rejoin the guest
 * after all of it, because the two loops together are just
 *     while (dst < end) { *dst++ = pal[lut[*src++]]; }
 * with the block doing 64 pixels a pass and the tail the remainder.  The pixel
 * counts agree exactly (64n + ceil(rest/4) == ceil((end-start)/4)), so one
 * merged loop reproduces both.
 *
 * Verification: the guard and the fixup/tail are compared byte-for-byte, and
 * each of the 64 unrolled groups is REBUILT from its index and compared - which
 * also pins the source and destination displacement progressions the merged
 * loop relies on, rather than trusting a reading of the disassembly.
 */
#define ND_SURFSPAN   0x519a41u
#define ND_SPAN_GRP   0x2au     /* the 64 unrolled groups   */
#define ND_SPAN_EPI   0x511u    /* fixup arithmetic + tail  */
#define ND_SPAN_EXIT  0x56du    /* where the two paths join */
#define ND_SPAN_PAL   (ND_SPAN_EPI + 69u)

static const uint8_t nd_span_pro[42] = {
    0x8d,0x83,0x00,0xff,0xff,0xff,0x89,0x85,0x2c,0xff,0xff,0xff,
    0x39,0xc1,0x0f,0x83,0x2c,0x05,0x00,0x00,0x89,0x9d,0x28,0xff,
    0xff,0xff,0x89,0xc8,0x89,0xf2,0x89,0x8d,0x24,0xff,0xff,0xff,
    0x8b,0x8d,0x2c,0xff,0xff,0xff,
};
static const uint8_t nd_span_epi[92] = {   /* +69..72 (the palette) is a wildcard */
    0x39,0xc8,0x0f,0x82,0x11,0xfb,0xff,0xff,0x8b,0x8d,0x24,0xff,
    0xff,0xff,0x8b,0x9d,0x28,0xff,0xff,0xff,0xb8,0xff,0xfe,0xff,
    0xff,0x29,0xc8,0x01,0xd8,0xc1,0xe8,0x08,0x83,0xc0,0x01,0x89,
    0xc2,0xc1,0xe0,0x08,0xc1,0xe2,0x07,0x01,0xc1,0x01,0xd6,0x39,
    0xd9,0x73,0x29,0x89,0xc8,0x0f,0xb7,0x16,0x83,0xc0,0x04,0x83,
    0xc6,0x02,0x0f,0xb6,0x14,0x17,0x8b,0x14,0x95,0x00,0x00,0x00,
    0x00,0x89,0x50,0xfc,0x39,0xd8,0x72,0xe5,0x8d,0x43,0xff,0x29,
    0xc8,0xc1,0xe8,0x02,0x8d,0x4c,0x81,0x04,
};

static int nd_span_skeleton_ok( uint32_t va, uint32_t *palout )
{
    uint32_t pal, o;
    unsigned i, k;
    static const uint8_t g0[] = { 0x0f,0xb7,0x1a, 0x05,0x00,0x01,0x00,0x00, 0x83,0xea,0x80 };

    for (i = 0; i < sizeof(nd_span_pro); i++)
        if (rd8( va + i ) != nd_span_pro[i]) return 0;
    for (i = 0; i < sizeof(nd_span_epi); i++)
        if ((i < 69 || i > 72) && rd8( va + ND_SPAN_EPI + i ) != nd_span_epi[i]) return 0;
    pal = rd32( va + ND_SPAN_PAL );

    o = va + ND_SPAN_GRP;
    for (k = 0; k < sizeof(g0); k++) if (rd8( o + k ) != g0[k]) return 0;
    o += sizeof(g0);
    for (i = 0; i < 64; i++)
    {
        int32_t dd = -0x100 + 4 * (int32_t)i;
        if (i)      /* group 0 reads (%edx); every later one uses a disp8 */
        {
            if (rd8( o ) != 0x0f || rd8( o+1 ) != 0xb7 || rd8( o+2 ) != 0x5a ||
                rd8( o+3 ) != (uint8_t)(2 * i - 128)) return 0;
            o += 4;
        }
        if (rd8( o ) != 0x0f || rd8( o+1 ) != 0xb6 ||
            rd8( o+2 ) != 0x1c || rd8( o+3 ) != 0x1f) return 0;       /* (%edi,%ebx,1) */
        o += 4;
        if (rd8( o ) != 0x8b || rd8( o+1 ) != 0x1c ||
            rd8( o+2 ) != 0x9d || rd32( o+3 ) != pal) return 0;       /* PAL(,%ebx,4)  */
        o += 7;
        if (dd >= -128)
        {
            if (rd8( o ) != 0x89 || rd8( o+1 ) != 0x58 || rd8( o+2 ) != (uint8_t)dd) return 0;
            o += 3;
        }
        else
        {
            if (rd8( o ) != 0x89 || rd8( o+1 ) != 0x98 || rd32( o+2 ) != (uint32_t)dd) return 0;
            o += 6;
        }
    }
    if (o != va + ND_SPAN_EPI) return 0;      /* the groups must exactly fill the gap */
    *palout = pal;
    return 1;
}

static int g_skip_blit;

static int nat_surfspan( struct x86cpu *c )
{
    uint32_t b   = ND_SURFSPAN + (uint32_t)nd_slide;
    uint32_t pal = rd32( b + ND_SPAN_PAL );
    uint32_t dst = c->regs[ECX], end = c->regs[EBX];
    uint32_t src = c->regs[ESI], lut = c->regs[EDI], ebp = c->regs[EBP];
    unsigned n = 0;

    /* the guest spills these before it branches and something later in this
     * 13KB function may read them, so write exactly what it would have */
    wr32( ebp - 0xd4, end - 0x100 );
    if (dst < end - 0x100) { wr32( ebp - 0xd8, end ); wr32( ebp - 0xdc, dst ); }

    if (g_skip_blit)
    {   /* Default: don't convert at all.  The game owes SDL a 32-bpp surface,
         * but on our path nobody ever reads it - we present the 8-bpp
         * frameplace ourselves at the frame flip - so this is 128,000 pixels a
         * frame of lookups for a buffer no one looks at.  Verified by playing
         * with it off: the picture is unchanged.  %ecx must still end where the
         * loop would have left it, since the caller walks scanlines with it.
         * WASM_KEEP_BLIT=1 restores the conversion. */
        n = (end - dst + 3) / 4;
        src += 2 * n; dst += 4 * n;
    }
    else while (dst < end)
    {
        wr32( dst, rd32( pal + 4u * rd8( lut + rd16( src ) ) ) );
        dst += 4; src += 2; n++;
    }
    g_sb_calls++; g_sb_iters += n;

    /* At the join %eax and %edx are dead (both reloaded before any use), %ebx
     * still holds `end`, and %esi is immediately overwritten with %ecx - so only
     * %ecx has to be right.  %esi is kept honest anyway. */
    c->regs[ECX] = dst; c->regs[ESI] = src;
    c->eip = b + ND_SPAN_EXIT;
    return 1;
}

static void nat_arm_surfspan( void )
{
    uint32_t va = ND_SURFSPAN + (uint32_t)nd_slide, pal = 0;
    if (nd_span_skeleton_ok( va, &pal )) nat_register( va, NAT_SURFSPAN, "surface span" );
    else fprintf( stderr, "wasm_x86: surface span skeleton differs at %08x - left interpreted\n", va );
}

/* ---- memoise libdivide's divider generator --------------------------------
 *
 * libdivide_internal_s64_gen(d, branchfree) turns a divisor into a magic
 * number, and after the two texture mappers went native it was the single
 * biggest cost left: ~479 calls a frame at ~278 interpreted instructions each,
 * 16% of the frame (12.7% in the generator, 3.6% in the __udivmoddi4 it calls -
 * a 64-bit division on a 32-bit guest is a long shift-subtract loop).
 *
 * It is a pure function, so it can be cached - but only if the divisors repeat.
 * A probe that simulated this exact cache in-place (declining every call, only
 * looking) measured a 78.5% hit rate over 2.0M calls, so they do.
 *
 * The tempting alternative was to reimplement the generator natively and skip
 * it entirely.  Deliberately not done: a magic number that is subtly wrong does
 * not crash, it quietly skews the renderer's perspective arithmetic, and
 * validating a libdivide implementation means validating its `do` side too.
 * Here the guest computes every value we cache, so a cached divider is by
 * construction exactly the one it would have produced; the cache can only be
 * wrong about WHICH divisor a value belongs to, and that is a plain key compare.
 *
 * Misses run the real function through a nested run() and keep the result -
 * hence the recursion guard, since the nested run re-enters at this same
 * address.  Inside another guest call (the audio mixer) nesting is refused, and
 * we simply decline and let the interpreter do it.
 */
#define ND_LIBDIV     0x401e60u
#define LD_SETS 4096
#define LD_WAYS 4
static const uint8_t nd_libdiv_code[] = {
    0x55,                       /* push %ebp                */
    0x89,0xe5,                  /* mov  %esp,%ebp           */
    0x57,                       /* push %edi                */
    0x89,0xcf,                  /* mov  %ecx,%edi           */
    0x56,                       /* push %esi                */
    0x53,                       /* push %ebx                */
    0x81,0xec,0x8c,0x00,0x00,0x00, /* sub $0x8c,%esp        */
    0x09,0xd7,                  /* or   %edx,%edi   (d == 0?) */
    0x89,0x45,0xd4,             /* mov  %eax,-0x2c(%ebp)  (the sret pointer) */
    0x89,0x4d,0xd0              /* mov  %ecx,-0x30(%ebp)  (d, high word)     */
};
struct ld_ent { uint64_t key; uint32_t lo, hi; uint8_t more, used; };
static struct ld_ent g_ld[LD_SETS][LD_WAYS];
static uint64_t g_ld_calls, g_ld_hits;
static int g_ld_filling, g_ld_verify;
static uint64_t g_ld_ok, g_ld_bad;
static uint64_t g_ld_last_key;
static uint32_t g_ld_last_lo, g_ld_last_hi;
static uint8_t g_ld_last_more, g_ld_last_valid;
static int g_ld_last_on = -1;

static int nat_libdivide( struct x86cpu *c )
{
    uint32_t esp  = c->regs[ESP];
    uint32_t sret = c->regs[EAX];
    uint32_t caller = rd32( esp );
    uint32_t bf, h;
    uint64_t key;
    int w;

    /* Compiled CON lives in the 0x03xxxxxx arena and can relocate while a
     * level is loading.  A cache miss there would use nested run(), which is
     * unsafe across that relocation.  Let the original guest function handle
     * dynamic-CON callers; ordinary executable callers still get the cache. */
    if (caller >= 0x03000000u && caller < 0x04000000u) return 0;
    if (g_ld_filling) return 0;                  /* the nested run re-enters here */
    bf  = rd32( esp + 4 );                       /* branchfree: the one stack arg */
    key = ((((uint64_t)c->regs[ECX] << 32) | c->regs[EDX]) << 1) | (bf != 0);
    h   = (uint32_t)((key * 0x9e3779b97f4a7c15ull) >> 52) & (LD_SETS - 1);

    g_ld_calls++;
    if (g_ld_last_on < 0) g_ld_last_on = getenv( "WASM_NO_LD_LAST" ) ? 0 : 1;
    if (g_ld_last_on && !g_ld_verify && g_ld_last_valid && g_ld_last_key == key)
    {
        wr32( sret, g_ld_last_lo ); wr32( sret + 4, g_ld_last_hi ); wr8( sret + 8, g_ld_last_more );
        g_ld_hits++;
        goto ret_to_caller;
    }
    for (w = 0; w < LD_WAYS; w++)
    {
        struct ld_ent *e = &g_ld[h][w];
        if (e->used && e->key == key)
        {
            if (g_ld_verify)
            {   /* differential check: run the real generator over the same
                 * arguments and compare.  This tests the things a wrong cache
                 * would get wrong - the key, the branchfree argument's location
                 * and the struct offsets - against the guest itself. */
                uint32_t vlo, vhi; uint8_t vmore; int ok;
                g_ld_filling = 1;
                ok = call_guest_cdecl( c, ND_LIBDIV + (uint32_t)nd_slide, 1, &bf, esp );
                g_ld_filling = 0;
                if (ok)
                {
                    vlo = rd32( sret ); vhi = rd32( sret + 4 ); vmore = rd8( sret + 8 );
                    if (vlo != e->lo || vhi != e->hi || vmore != e->more)
                    {
                        if (g_ld_bad++ < 8)
                            fprintf( stderr, "wasm_x86: LIBDIV MISMATCH d=%08x%08x bf=%u "
                                     "cached %08x%08x/%02x real %08x%08x/%02x\n",
                                     c->regs[ECX], c->regs[EDX], bf,
                                     e->hi, e->lo, e->more, vhi, vlo, vmore );
                    }
                    else g_ld_ok++;
                }
            }
            wr32( sret, e->lo ); wr32( sret + 4, e->hi ); wr8( sret + 8, e->more );
            g_ld_last_key = e->key; g_ld_last_lo = e->lo; g_ld_last_hi = e->hi;
            g_ld_last_more = e->more; g_ld_last_valid = 1;
            if (w) { struct ld_ent t = *e; g_ld[h][w] = g_ld[h][w-1]; g_ld[h][w-1] = t; }
            g_ld_hits++;
            goto ret_to_caller;
        }
    }
    {   /* miss: let the real generator run, then remember what it produced */
        int ok;
        g_ld_filling = 1;
        ok = call_guest_cdecl( c, ND_LIBDIV + (uint32_t)nd_slide, 1, &bf, esp );
        g_ld_filling = 0;
        if (!ok) return 0;                       /* already inside a guest call */
        for (w = LD_WAYS - 1; w > 0; w--) g_ld[h][w] = g_ld[h][w-1];
        g_ld[h][0].key  = key;
        g_ld[h][0].lo   = rd32( sret );
        g_ld[h][0].hi   = rd32( sret + 4 );
        g_ld[h][0].more = rd8 ( sret + 8 );
        g_ld[h][0].used = 1;
        g_ld_last_key = key; g_ld_last_lo = g_ld[h][0].lo; g_ld_last_hi = g_ld[h][0].hi;
        g_ld_last_more = g_ld[h][0].more; g_ld_last_valid = 1;
    }
ret_to_caller:
    c->regs[EAX] = sret;                         /* the generator returns the sret ptr */
    c->regs[ESP] = esp + 4;                      /* pop the return address */
    c->eip = rd32( esp );
    return 1;
}

static void nat_arm_libdivide( void )
{
    uint32_t va = ND_LIBDIV + (uint32_t)nd_slide;
    unsigned i;
    for (i = 0; i < sizeof(nd_libdiv_code); i++)
        if (rd8( va + i ) != nd_libdiv_code[i])
        { fprintf( stderr, "wasm_x86: libdivide gen differs at %08x - left interpreted\n", va ); return; }
    nat_register( va, NAT_LIBDIV, "libdivide gen cache" );
}

/* Read-only msvcrt calls are common around the renderer's asset and command
 * paths.  Keep these bounded to the guest address space and exact about the
 * byte-wise result, then use the same cdecl return convention as the existing
 * memcpy/memmove fast paths. */
#define NAT_GUEST_END 0x70000000u
static int nat_ret_eax( struct x86cpu *c, uint32_t esp, uint32_t value )
{
    c->regs[EAX] = value;
    c->regs[ESP] = esp + 4;
    c->eip = rd32( esp );
    return 1;
}

/* SDL's REAL atomic getters are tiny cdecl wrappers around one guest-memory
 * load.  They are called heavily by the renderer's lock-free queues.  Use a
 * sequentially consistent host load so the shortcut retains the ordering
 * promised by the original x86 instruction, while refusing addresses outside
 * the guest linear-memory range. */
#define ND_SDL_ATOMIC_GET     0x73cac0u
#define ND_SDL_ATOMIC_GETPTR  0x73cad0u
#define ND_SDL_ATOMIC_XADD    0x73cab0u
static int nat_sdl_atomic_get( struct x86cpu *c )
{
    uint32_t sp = c->regs[ESP], p = rd32( sp + 4 );
    if (p > NAT_GUEST_END - 4u) return 0;
    return nat_ret_eax( c, sp, __atomic_load_n( (uint32_t *)(uintptr_t)p, __ATOMIC_SEQ_CST ) );
}

/* SDL_TLSGet_REAL is a cdecl wrapper around SDL_SYS_GetTLSData.  Once SDL has
 * initialized its per-thread array, the common path is only the bounds check
 * and one indexed load; the initialization/slow path must remain in SDL. */
#define ND_SDL_TLSGET 0x6ff720u
#define ND_SDL_TLSKEY 0x831900u
static int nat_sdl_tlsget( struct x86cpu *c )
{
    uint32_t sp = c->regs[ESP], key = rd32( sp + 4 );
    uint32_t data = rd32( ND_SDL_TLSKEY + (uint32_t)nd_slide );
    uint32_t count, slot;

    if (!data || !key || data >= NAT_GUEST_END - 4u) return 0;
    count = rd32( data );
    if (key > count || key > (NAT_GUEST_END - data + 4u) / 8u) return 0;
    slot = data + key * 8u - 4u;
    if (slot < data || slot >= NAT_GUEST_END - 4u) return 0;
    return nat_ret_eax( c, sp, rd32( slot ) );
}

static void nat_arm_sdl_tlsget( void )
{
    uint32_t b = ND_SDL_TLSGET + (uint32_t)nd_slide;
    /* The call displacement is relocation-dependent; the wrapper body and
     * its branch/load/return sequence are the ABI guard. */
    static const uint8_t code[] = {
        0x53, 0x8b,0x5c,0x24,0x08, 0xe8,0,0,0,0, 0x85,0xc0,0x74,0x12,
        0x85,0xdb,0x74,0x0e,0x39,0x18,0x72,0x0a,0x8b,0x44,0xd8,0xfc,
        0x5b,0xc3
    };
    unsigned i;
    for (i = 0; i < sizeof(code); i++)
        if (i < 5 || i > 9)
            if (rd8( b + i ) != code[i])
            { fprintf( stderr, "wasm_x86: SDL_TLSGet_REAL skeleton differs at %08x+%u (got %02x want %02x) - left interpreted\n",
                        b, i, rd8( b + i ), code[i] ); return; }
    if (!getenv( "WASM_NO_SDL_TLSGET" ))
        nat_register( b, NAT_SDL_TLSGET, "SDL_TLSGet_REAL cached path" );
}

static int nat_sdl_atomic_xadd( struct x86cpu *c )
{
    uint32_t sp = c->regs[ESP], p = rd32( sp + 4 ), v = rd32( sp + 8 );
    if (p > NAT_GUEST_END - 4u) return 0;
    return nat_ret_eax( c, sp,
        __atomic_fetch_add( (uint32_t *)(uintptr_t)p, v, __ATOMIC_SEQ_CST ) );
}

/* Fast path for ntdll's stdcall RtlAcquireSRWLockExclusive.  The original
 * implementation waits when the upper waiter field is nonzero.  Declining in
 * that case is important: the interpreter remains responsible for the wait
 * protocol, while the common uncontended case is one host atomic CAS. */
static int nat_ntdll_srw_excl( struct x86cpu *c )
{
    uint32_t sp = c->regs[ESP], p = rd32( sp + 4 );
    if ((p & 3u) || p > NAT_GUEST_END - 4u) return 0;
    uint32_t old = __atomic_load_n( (uint32_t *)(uintptr_t)p, __ATOMIC_SEQ_CST );
    if (old != 0) return 0;                      /* no waiters or owners */
    uint32_t expected = old;
    if (!__atomic_compare_exchange_n( (uint32_t *)(uintptr_t)p, &expected,
                                      0x00010001u, 0, __ATOMIC_SEQ_CST,
                                      __ATOMIC_SEQ_CST )) return 0;
    c->eip = rd32( sp );                         /* stdcall: ret 4 */
    c->regs[ESP] = sp + 8;
    return 1;
}

static int nat_ntdll_srw_shared( struct x86cpu *c )
{
    uint32_t sp = c->regs[ESP], p = rd32( sp + 4 );
    if ((p & 3u) || p > NAT_GUEST_END - 4u) return 0;
    uint32_t old = __atomic_load_n( (uint32_t *)(uintptr_t)p, __ATOMIC_SEQ_CST );
    if (old & 0xffffu) return 0;                  /* exclusive/waiter state */
    uint32_t expected = old;
    if (!__atomic_compare_exchange_n( (uint32_t *)(uintptr_t)p, &expected,
                                      old + 0x00010000u, 0, __ATOMIC_SEQ_CST,
                                      __ATOMIC_SEQ_CST )) return 0;
    c->eip = rd32( sp );                           /* stdcall: ret 4 */
    c->regs[ESP] = sp + 8;
    return 1;
}

static int nat_ntdll_srw_excl_rel( struct x86cpu *c )
{
    uint32_t sp = c->regs[ESP], p = rd32( sp + 4 );
    if ((p & 3u) || p > NAT_GUEST_END - 4u) return 0;
    uint32_t expected = 0x00010001u;
    if (!__atomic_compare_exchange_n( (uint32_t *)(uintptr_t)p, &expected, 0,
                                      0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST )) return 0;
    c->eip = rd32( sp );
    c->regs[ESP] = sp + 8;
    return 1;
}

static int nat_ntdll_wake_all_empty( struct x86cpu *c )
{
    uint32_t sp = c->regs[ESP], p = rd32( sp + 4 );
    if (!p || p > NAT_GUEST_END - 1u || !g_ntdll_wake_buckets) return 0;
    uint32_t bucket = g_ntdll_wake_buckets + (((p >> 4) & 0xffu) * 12u);
    uint32_t lock = bucket + 8u, expected = 0;
    if (!__atomic_compare_exchange_n( (uint32_t *)(uintptr_t)lock, &expected,
                                      0xffffffffu, 0, __ATOMIC_SEQ_CST,
                                      __ATOMIC_SEQ_CST )) return 0;
    int empty = rd32( bucket ) == 0;
    __atomic_store_n( (uint32_t *)(uintptr_t)lock, 0, __ATOMIC_SEQ_CST );
    if (!empty) return 0;
    c->eip = rd32( sp );                           /* stdcall: ret 4 */
    c->regs[ESP] = sp + 8;
    return 1;
}

static int nat_arm_sdl_atomics( void )
{
    static const uint8_t code[] = { 0x8b,0x44,0x24,0x04,0x8b,0x00,0xc3 };
    static const uint8_t xadd[] = { 0x8b,0x54,0x24,0x04,0x8b,0x44,0x24,0x08,
                                    0xf0,0x0f,0xc1,0x02,0xc3 };
    uint32_t get = ND_SDL_ATOMIC_GET + (uint32_t)nd_slide;
    uint32_t ptr = ND_SDL_ATOMIC_GETPTR + (uint32_t)nd_slide;
    uint32_t add = ND_SDL_ATOMIC_XADD + (uint32_t)nd_slide;
    unsigned i; int gok = 1, pok = 1;
    for (i = 0; i < sizeof(code); i++)
    { if (rd8( get + i ) != code[i]) gok = 0; if (rd8( ptr + i ) != code[i]) pok = 0; }
    if (gok && !getenv( "WASM_NO_SDL_ATOMIC_GET" ))
        nat_register( get, NAT_SDL_ATOMIC_GET, "SDL_AtomicGet_REAL" );
    if (pok && !getenv( "WASM_NO_SDL_ATOMIC_GET" ))
        nat_register( ptr, NAT_SDL_ATOMIC_GETPTR, "SDL_AtomicGetPtr_REAL" );
    if (!getenv( "WASM_NO_SDL_ATOMIC_XADD" ))
    {
        int ok = 1;
        for (i = 0; i < sizeof(xadd); i++) if (rd8( add + i ) != xadd[i]) ok = 0;
        if (ok) nat_register( add, NAT_SDL_ATOMIC_XADD, "SDL_AtomicAdd_REAL" );
        else fprintf( stderr, "wasm_x86: SDL_AtomicAdd skeleton differs at %08x - left interpreted\n", add );
    }
    return gok && pok;
}

static int nat_memcmp( struct x86cpu *c )
{
    uint32_t esp = c->regs[ESP], a = rd32( esp + 4 ), b = rd32( esp + 8 );
    uint32_t n = rd32( esp + 12 ), i;
    if (n > NAT_GUEST_END || a > NAT_GUEST_END - n || b > NAT_GUEST_END - n) return 0;
    for (i = 0; i < n; i++)
    {
        uint8_t x = rd8( a + i ), y = rd8( b + i );
        if (x != y) return nat_ret_eax( c, esp, (uint32_t)(int)x - (uint32_t)(int)y );
    }
    return nat_ret_eax( c, esp, 0 );
}

static int nat_strcmp( struct x86cpu *c )
{
    uint32_t esp = c->regs[ESP], a = rd32( esp + 4 ), b = rd32( esp + 8 );
    if (a >= NAT_GUEST_END || b >= NAT_GUEST_END) return 0;
    for (;; a++, b++)
    {
        uint8_t x, y;
        if (a >= NAT_GUEST_END || b >= NAT_GUEST_END) return 0;
        x = rd8( a ); y = rd8( b );
        if (x != y) return nat_ret_eax( c, esp, (uint32_t)(int)x - (uint32_t)(int)y );
        if (!x) return nat_ret_eax( c, esp, 0 );
    }
}

static int nat_strlen( struct x86cpu *c )
{
    uint32_t esp = c->regs[ESP], a = rd32( esp + 4 ), p;
    if (a >= NAT_GUEST_END) return 0;
    for (p = a; p < NAT_GUEST_END; p++)
        if (!rd8( p )) return nat_ret_eax( c, esp, p - a );
    return 0;
}

static int nat_wcslen( struct x86cpu *c )
{
    uint32_t esp = c->regs[ESP], a = rd32( esp + 4 ), p;
    if (a >= NAT_GUEST_END || a & 1) return 0;
    for (p = a; p + 1 < NAT_GUEST_END; p += 2)
        if (!rd16( p )) return nat_ret_eax( c, esp, (p - a) / 2 );
    return 0;
}

static int nat_wcschr( struct x86cpu *c )
{
    uint32_t esp = c->regs[ESP], a = rd32( esp + 4 ), ch = rd32( esp + 8 ) & 0xffffu, p;
    if (a >= NAT_GUEST_END || (a & 1)) return 0;
    for (p = a; p + 1 < NAT_GUEST_END; p += 2)
    {
        uint32_t x = rd16( p );
        if (x == ch) return nat_ret_eax( c, esp, p );
        if (!x) return nat_ret_eax( c, esp, 0 );
    }
    return 0;
}

/* Entry is the verified movaps loop inside msvcrt memset, reached when a
 * translated/interpreted caller lands at the loop body instead of the export
 * boundary. EAX/EDX hold the repeated four-byte pattern, ESI is the current
 * destination, and EBX is the exclusive end. Leave the machine at memset's
 * epilogue so its original return convention and result are preserved. */
static int nat_memset_loop( struct x86cpu *c )
{
    uint32_t cur = c->regs[ESI], end = c->regs[EBX], eax = c->regs[EAX], edx = c->regs[EDX];
    if (cur >= NAT_GUEST_END || end > NAT_GUEST_END || end < cur || ((end - cur) & 31u)) return 0;
    while (cur < end)
    {
        wr32( cur +  0, eax ); wr32( cur +  4, edx );
        wr32( cur +  8, eax ); wr32( cur + 12, edx );
        wr32( cur + 16, eax ); wr32( cur + 20, edx );
        wr32( cur + 24, eax ); wr32( cur + 28, edx );
        cur += 32;
    }
    c->regs[ESI] = cur;
    c->eip = msvcrt_base + 0x49b8eu;
    return 1;
}

static void jit_xmm_store( struct x86cpu *c, int src, uint32_t a, int n );
static inline __attribute__((always_inline)) int cond( struct x86cpu *c, int cc );

/* The loop is sometimes entered at an instruction boundary inside the
 * movaps sequence by a translated caller.  Those four addresses intentionally
 * cannot occupy nat_register's direct table independently: they all hash to
 * the same 16-byte slot as the verified loop entry.  Handle them only when
 * that slot is already owned by the loop, and only after the exact skeleton
 * above has been checked. */
static int nat_memset_loop_tail( struct x86cpu *c )
{
    uint32_t off = c->eip - g_msvcrt_memset_loop;
    uint32_t cur = c->regs[ESI], end = c->regs[EBX];
    if (off == 12) {
        /* The preceding `cmp esi,ebx` has already produced the flags used by
         * this `jb`; do not recompute them or disturb lazy-flag state. */
        c->eip = cond( c, 2 ) ? g_msvcrt_memset_loop : msvcrt_base + 0x49b8eu;
        return 1;
    }
    if (off != 3 && off != 6 && off != 10) return 0;
    if (cur >= NAT_GUEST_END || end > NAT_GUEST_END || end < cur) return 0;
    if (off == 3) { cur += 32; c->regs[ESI] = cur; }
    if (off == 3 || off == 6) {
        if (cur < 16 || cur > NAT_GUEST_END) return 0;
        jit_xmm_store( c, 0, cur - 16, 16 );
    }
    set_lazy( c, K_SUB, cur, end, cur - end, 4 );
    c->eip = cond( c, 2 ) ? g_msvcrt_memset_loop : msvcrt_base + 0x49b8eu;
    return 1;
}

/* Runtime-generated CON dispatch glue uses absolute indirect jumps.  This is
 * miss-only and restricted to the generated-code arena; for ff 25 the encoded
 * immediate is a pointer to a guest memory slot, so perform both dereferences
 * just as the interpreter's ModRM path does. */
static int nat_dynamic_jmp( struct x86cpu *c )
{
    uint32_t a = c->eip, p;
    static int enabled = -1;
    static int trace = -1;
    static unsigned trace_n;
    static int div64_enabled = -1;
    static uint32_t div64_addr;
    static unsigned div64_hits;
    if (enabled < 0) enabled = getenv( "WASM_NO_DYNAMIC_THUNK" ) ? 0 : 1;
    if (!enabled) return 0;
    /* Runtime-generated CON helpers end at ordinary near returns.  These
     * instructions are fully self-contained: the interpreter's implementation
     * only reads the stack return address (and, for C2, the immediate cleanup)
     * before continuing at it.  Handle them directly in the generated arena to
     * avoid re-entering the large opcode decoder for every tiny helper exit.
     * Keep this independently switchable because generated code is self-
     * modifying and the range guard is part of the safety contract. */
    /* Runtime-generated CON code is self-modifying.  Although the c3/c2
     * shortcut is instruction-for-instruction equivalent for stable code,
     * treating every return-shaped byte in this broad arena as a thunk can
     * consume a transient/generated entry with the wrong stack contract.
     * That strands the main thread at a data byte during level loading.  Keep
     * the ordinary interpreter return semantics as the safe default; the
     * shortcut remains available for targeted benchmarking. */
    if (getenv( "WASM_DYNAMIC_RET" ) && !getenv( "WASM_NO_DYNAMIC_RET" ) &&
        a >= 0x00800000u && a < 0x01000000u)
    {
        uint32_t sp = c->regs[ESP], n = 0;
        uint8_t op = rd8( a );
        if (op == 0xc2) n = rd16( a + 1u );
        else if (op != 0xc3) n = 0xffffffffu;
        if (n != 0xffffffffu && sp <= NAT_GUEST_END - 4u - n)
        {
            c->eip = rd32( sp );
            c->regs[ESP] = sp + 4u + n;
            return 1;
        }
    }
    if (div64_enabled < 0) div64_enabled = getenv( "WASM_NO_NATIVE_DIV64" ) ? 0 : 1;
    if (div64_enabled && a == 0x008066f0u &&
        rd8(a+47) == 0xe8)
    {
        div64_addr = a + 52u + rd32(a + 48);
        if (div64_hits == 0) fprintf( stderr, "wasm_x86: DIV_ARM target=%08x\n", div64_addr );
    }
    if (div64_enabled && div64_addr && a == div64_addr &&
        rd8(a+0) == 0x55 && rd8(a+1) == 0x57 && rd8(a+2) == 0x56 &&
        rd8(a+3) == 0x53 && rd8(a+4) == 0x83 && rd8(a+5) == 0xec &&
        rd8(a+6) == 0x2c && rd8(a+7) == 0x8b && rd8(a+8) == 0x54 &&
        rd8(a+9) == 0x24 && rd8(a+10) == 0x44 && rd8(a+11) == 0x8b &&
        rd8(a+12) == 0x44 && rd8(a+13) == 0x24 && rd8(a+14) == 0x40 &&
        rd8(a+15) == 0xc7 && rd8(a+16) == 0x44 && rd8(a+17) == 0x24 &&
        rd8(a+18) == 0x04 && rd8(a+19) == 0x00 && rd8(a+20) == 0x00 &&
        rd8(a+21) == 0x00 && rd8(a+22) == 0x00)
    {
        uint32_t sp = c->regs[ESP], nlo = rd32(sp+4), nhi = rd32(sp+8);
        uint32_t dlo = rd32(sp+12), dhi = rd32(sp+16), out = rd32(sp+20);
        uint64_t n = (uint64_t)nlo | ((uint64_t)nhi << 32);
        uint64_t d = (uint64_t)dlo | ((uint64_t)dhi << 32), q, r;
        int ns = (nhi >> 31) != 0, ds = (dhi >> 31) != 0;
        if (!d || out < 0x10000u || out > NAT_GUEST_END - 8u) return 0;
        if (ns) n = 0 - n;
        if (ds) d = 0 - d;
        q = n / d; r = n % d;
        if (ns) r = 0 - r;
        if (ns != ds) q = 0 - q;
        wr32(out, (uint32_t)r); wr32(out+4, (uint32_t)(r >> 32));
        c->regs[EAX] = (uint32_t)q; c->regs[EDX] = (uint32_t)(q >> 32);
        c->eip = rd32(sp); c->regs[ESP] = sp + 4;
        if (++div64_hits == 1) fprintf( stderr, "wasm_x86: DIV_HIT target=%08x\n", a );
        return 1;
    }
    if (trace < 0) trace = getenv( "WASM_TRACE_DYNAMIC" ) ? 1 : 0;
    if (trace && g_flip_count > 100 && a >= 0x00800000u && a < 0x01000000u && trace_n < 128) {
        fprintf( stderr, "DYNAMIC_MISS eip=%08x prev=%08x esp=%08x bytes=",
                 a, c->eip, c->regs[ESP] );
        for (unsigned i = 0; i < 64; i++) fprintf( stderr, "%02x", rd8(a+i) );
        if (a == 0x008066f0u && rd8(a+47) == 0xe8)
            fprintf( stderr, " target=%08x", a + 52u + rd32(a + 48) );
        fprintf( stderr, "\n" );
        trace_n++;
    }
    /* Small runtime feature predicate called from the generated CON loop:
     *   xor eax,eax; cmp [f9922c],0; je ret; cmp [829e09],0; je ret;
     *   cmp [829e08],0; je ret; edx=[dbfa60]; test edx,edx; je ret;
     *   test byte [edx+494],4; jne ret; eax=[dde944]; ret
     * Keep the conditional flags as well as the cdecl return value. */
    if (a == 0x004da630u &&
        rd8(a) == 0x31 && rd8(a + 1) == 0xc0 &&
        rd8(a + 2) == 0x80 && rd8(a + 3) == 0x3d &&
        rd32(a + 4) == 0x00f9922cu && rd8(a + 8) == 0x00 &&
        rd8(a + 9) == 0x74 && rd8(a + 10) == 0x2a &&
        rd8(a + 11) == 0x80 && rd8(a + 12) == 0x3d &&
        rd32(a + 13) == 0x00829e09u && rd8(a + 17) == 0x00 &&
        rd8(a + 18) == 0x74 && rd8(a + 19) == 0x21 &&
        rd8(a + 20) == 0x80 && rd8(a + 21) == 0x3d &&
        rd32(a + 22) == 0x00829e08u && rd8(a + 26) == 0x00 &&
        rd8(a + 27) == 0x74 && rd8(a + 28) == 0x18)
    {
        uint32_t v;
        c->regs[EAX] = 0;
        set_lazy( c, K_LOGIC, 0, 0, 0, 4 );
        v = rd8( 0x00f9922cu );
        set_lazy( c, K_SUB, v, 0, v, 1 );
        if (v == 0) goto feature_predicate_return;
        v = rd8( 0x00829e09u );
        set_lazy( c, K_SUB, v, 0, v, 1 );
        if (v == 0) goto feature_predicate_return;
        v = rd8( 0x00829e08u );
        set_lazy( c, K_SUB, v, 0, v, 1 );
        if (v == 0) goto feature_predicate_return;
        v = rd32( 0x00dbfa60u );
        set_lazy( c, K_LOGIC, v, v, v, 4 );
        if (v == 0) goto feature_predicate_return;
        if (v > NAT_GUEST_END - 0x498u) return 0;
        v = rd8( v + 0x494u );
        set_lazy( c, K_LOGIC, v, 4, v & 4u, 1 );
        if (v & 4u) goto feature_predicate_return;
        c->regs[EAX] = rd32( 0x00dde944u );
    feature_predicate_return:
        c->eip = rd32( c->regs[ESP] );
        c->regs[ESP] += 4u;
        return 1;
    }
    /* The CON compiler emits this stable wrapper around the already-translated
     * bit-index helper at 0073ac20.  Keep the wrapper's ABI-visible effects,
     * then enter the translated callee directly.  The continuation starts at
     * +0x10 (the TEST EAX instruction) after the five-byte CALL. */
    if (a >= 0x006f33a0u && a < 0x006f33a4u &&
        rd8(a) == 0x53 && rd8(a + 1) == 0x83 && rd8(a + 2) == 0xec &&
        rd8(a + 3) == 0x04 && rd8(a + 4) == 0xc7 && rd8(a + 5) == 0x04 &&
        rd8(a + 6) == 0x24 && rd32(a + 7) == 0x00008000u &&
        rd8(a + 11) == 0xe8 && rd32(a + 12) == 0x00047870u &&
        rd8(0x0073ac20u + (uint32_t)nd_slide) == 0x57)
    {
        uint32_t oldesp = c->regs[ESP];
        if (oldesp >= 0x40u && oldesp <= NAT_GUEST_END - 0x20u)
        {
            wr32( oldesp - 4u, c->regs[EBX] );
            c->regs[ESP] = oldesp - 8u;
            set_lazy( c, K_SUB, oldesp - 4u, 4u, oldesp - 8u, 4 );
            wr32( c->regs[ESP], 0x8000u );
            c->regs[ESP] -= 4u;
            wr32( c->regs[ESP], a + 16u );
            c->eip = 0x0073ac20u + (uint32_t)nd_slide;
            return 1;
        }
    }
    /* Wine's hot shared-user-data helper is a stack-neutral prologue around
     * one absolute load:
     *   mov edi,edi; push ebp; mov ebp,esp; mov eax,[0x7ffe0320];
     *   pop ebp; ret
     * The +2 entry is also reached through an indirect function pointer. */
    if ((rd8(a) == 0x8b && rd8(a+1) == 0xff && rd8(a+2) == 0x55 &&
         rd8(a+3) == 0x8b && rd8(a+4) == 0xec && rd8(a+5) == 0xa1 &&
         rd32(a+6) == 0x7ffe0320u && rd8(a+10) == 0x5d && rd8(a+11) == 0xc3) ||
        (rd8(a) == 0x55 && rd8(a+1) == 0x8b && rd8(a+2) == 0xec &&
         rd8(a+3) == 0xa1 && rd32(a+4) == 0x7ffe0320u &&
         rd8(a+8) == 0x5d && rd8(a+9) == 0xc3))
    {
        c->regs[EAX] = rd32( 0x7ffe0320u );
        c->eip = rd32( c->regs[ESP] );
        c->regs[ESP] += 4u;
        return 1;
    }
    /* Continuation of the shuffle/call veneer above.  Its fixed epilogue is
     * reached after the callee returns at +36:
     *   mov [ebx],eax; mov [ebx+4],edx; mov eax,[esp+28];
     *   mov edx,[esp+2c]; mov [ebx+8],eax; mov eax,ebx;
     *   mov [ebx+c],edx; add esp,38; pop ebx; ret
     * Match both the tail and the preceding call site so an unrelated ret
     * sequence in generated code cannot enter this shortcut. */
    if (a >= 0x00800000u + 36u && a < 0x01000000u &&
        rd8(a) == 0x89 && rd8(a+1) == 0x03 &&
        rd8(a+2) == 0x89 && rd8(a+3) == 0x53 && rd8(a+4) == 0x04 &&
        rd8(a+5) == 0x8b && rd8(a+6) == 0x44 && rd8(a+7) == 0x24 && rd8(a+8) == 0x28 &&
        rd8(a+9) == 0x8b && rd8(a+10) == 0x54 && rd8(a+11) == 0x24 && rd8(a+12) == 0x2c &&
        rd8(a+13) == 0x89 && rd8(a+14) == 0x43 && rd8(a+15) == 0x08 &&
        rd8(a+16) == 0x89 && rd8(a+17) == 0xd8 &&
        rd8(a+18) == 0x89 && rd8(a+19) == 0x53 && rd8(a+20) == 0x0c &&
        rd8(a+21) == 0x83 && rd8(a+22) == 0xc4 && rd8(a+23) == 0x38 &&
        rd8(a+24) == 0x5b && rd8(a+25) == 0xc3 &&
        rd8(a-5) == 0xe8 &&
        ((rd8(a-36) == 0x89 && rd8(a-32) == 0x8b &&
          rd8(a-28) == 0x89 && rd8(a-24) == 0x8b) ||
         (a >= 0x00800000u + 52u && rd8(a-52) == 0x53 &&
          rd8(a-51) == 0x83 && rd8(a-50) == 0xec && rd8(a-49) == 0x38 &&
          rd8(a-5) == 0xe8)))
    {
        uint32_t esp = c->regs[ESP], ebx = c->regs[EBX];
        if (ebx <= NAT_GUEST_END - 0x10u && esp <= NAT_GUEST_END - 0x30u)
        {
            wr32( ebx, c->regs[EAX] );
            wr32( ebx + 4u, c->regs[EDX] );
            c->regs[EAX] = rd32( esp + 0x28u );
            c->regs[EDX] = rd32( esp + 0x2cu );
            wr32( ebx + 8u, c->regs[EAX] );
            c->regs[EAX] = ebx;
            wr32( ebx + 0x0cu, c->regs[EDX] );
            set_lazy( c, K_ADD, esp, 0x38u, esp + 0x38u, 4 );
            esp += 0x38u;
            c->regs[EBX] = rd32( esp );
            c->eip = rd32( esp + 4u );
            c->regs[ESP] = esp + 8u;
            return 1;
        }
    }
    /* Sibling veneer with a small stack frame and the same call/epilogue
     * layout.  The sub instruction is included in the lazy flags, since its
     * result may be observed by the called generated code. */
    if (a >= 0x00800000u && a < 0x01000000u &&
        rd8(a) == 0x53 && rd8(a+1) == 0x83 && rd8(a+2) == 0xec && rd8(a+3) == 0x38 &&
        rd8(a+4) == 0x8d && rd8(a+5) == 0x44 && rd8(a+6) == 0x24 && rd8(a+7) == 0x28 &&
        rd8(a+8) == 0x8b && rd8(a+9) == 0x54 && rd8(a+10) == 0x24 && rd8(a+11) == 0x50 &&
        rd8(a+12) == 0x8b && rd8(a+13) == 0x5c && rd8(a+14) == 0x24 && rd8(a+15) == 0x40 &&
        rd8(a+16) == 0x89 && rd8(a+17) == 0x44 && rd8(a+18) == 0x24 && rd8(a+19) == 0x10 &&
        rd8(a+20) == 0x8b && rd8(a+21) == 0x44 && rd8(a+22) == 0x24 && rd8(a+23) == 0x4c &&
        rd8(a+24) == 0x89 && rd8(a+25) == 0x54 && rd8(a+26) == 0x24 && rd8(a+27) == 0x0c &&
        rd8(a+28) == 0x8b && rd8(a+29) == 0x54 && rd8(a+30) == 0x24 && rd8(a+31) == 0x48 &&
        rd8(a+32) == 0x89 && rd8(a+33) == 0x44 && rd8(a+34) == 0x24 && rd8(a+35) == 0x08 &&
        rd8(a+36) == 0x8b && rd8(a+37) == 0x44 && rd8(a+38) == 0x24 && rd8(a+39) == 0x44 &&
        rd8(a+40) == 0x89 && rd8(a+41) == 0x54 && rd8(a+42) == 0x24 && rd8(a+43) == 0x04 &&
        rd8(a+44) == 0x89 && rd8(a+45) == 0x04 && rd8(a+46) == 0x24 && rd8(a+47) == 0xe8)
    {
        uint32_t oldesp = c->regs[ESP], esp, target = a + 52u + rd32(a + 48);
        if (oldesp >= 0x3cu && oldesp <= NAT_GUEST_END - 0x50u &&
            target >= 0x00800000u && target < 0x01000000u)
        {
            wr32( oldesp - 4u, c->regs[EBX] );
            esp = oldesp - 0x3cu;
            c->regs[ESP] = esp;
            set_lazy( c, K_SUB, oldesp - 4u, 0x38u, esp, 4 );
            c->regs[EAX] = esp + 0x28u;
            c->regs[EDX] = rd32( esp + 0x50u );
            c->regs[EBX] = rd32( esp + 0x40u );
            wr32( esp + 0x10u, c->regs[EAX] );
            c->regs[EAX] = rd32( esp + 0x4cu );
            wr32( esp + 0x0cu, c->regs[EDX] );
            c->regs[EDX] = rd32( esp + 0x48u );
            wr32( esp + 0x08u, c->regs[EAX] );
            c->regs[EAX] = rd32( esp + 0x44u );
            wr32( esp + 0x04u, c->regs[EDX] );
            wr32( esp, c->regs[EAX] );
            esp -= 4u;
            wr32( esp, a + 52u );
            c->regs[ESP] = esp;
            c->eip = target;
            return 1;
        }
    }
    /* The runtime CON compiler emits this hot, flag-neutral argument shuffle
     * before a direct call:
     *   mov [esp+10],eax; mov eax,[esp+4c]; mov [esp+c],edx;
     *   mov edx,[esp+48]; mov [esp+8],eax; mov eax,[esp+44];
     *   mov [esp+4],edx; mov [esp],eax; call rel32
     * It lives in the 0080… generated-code arena, outside the DLL thunk range
     * below.  Execute the moves directly, then retain normal x86 call/return
     * behavior by pushing the exact post-call EIP. */
    if (a >= 0x00800000u && a < 0x01000000u &&
        rd8(a) == 0x89 && rd8(a+1) == 0x44 && rd8(a+2) == 0x24 && rd8(a+3) == 0x10 &&
        rd8(a+4) == 0x8b && rd8(a+5) == 0x44 && rd8(a+6) == 0x24 && rd8(a+7) == 0x4c &&
        rd8(a+8) == 0x89 && rd8(a+9) == 0x54 && rd8(a+10) == 0x24 && rd8(a+11) == 0x0c &&
        rd8(a+12) == 0x8b && rd8(a+13) == 0x54 && rd8(a+14) == 0x24 && rd8(a+15) == 0x48 &&
        rd8(a+16) == 0x89 && rd8(a+17) == 0x44 && rd8(a+18) == 0x24 && rd8(a+19) == 0x08 &&
        rd8(a+20) == 0x8b && rd8(a+21) == 0x44 && rd8(a+22) == 0x24 && rd8(a+23) == 0x44 &&
        rd8(a+24) == 0x89 && rd8(a+25) == 0x54 && rd8(a+26) == 0x24 && rd8(a+27) == 0x04 &&
        rd8(a+28) == 0x89 && rd8(a+29) == 0x04 && rd8(a+30) == 0x24 && rd8(a+31) == 0xe8)
    {
        uint32_t esp = c->regs[ESP];
        uint32_t target = a + 36u + rd32( a + 32 );
        if (esp >= 4u && esp <= NAT_GUEST_END - 0x50u &&
            target >= 0x00800000u && target < 0x01000000u)
        {
            wr32( esp + 0x10, c->regs[EAX] );
            c->regs[EAX] = rd32( esp + 0x4c );
            wr32( esp + 0x0c, c->regs[EDX] );
            c->regs[EDX] = rd32( esp + 0x48 );
            wr32( esp + 0x08, c->regs[EAX] );
            c->regs[EAX] = rd32( esp + 0x44 );
            wr32( esp + 0x04, c->regs[EDX] );
            wr32( esp, c->regs[EAX] );
            esp -= 4u;
            wr32( esp, a + 36u );
            c->regs[ESP] = esp;
            c->eip = target;
            return 1;
        }
    }
    if (a < 0x30000000u || a >= 0x40000000u) return 0;
    /* Return from the callback veneer above.  The dispatcher has already
     * popped the synthetic call return address, so this exact wrapper tail
     * performs the original stdcall return and argument cleanup. */
    if (a >= 12u && rd8(a) == 0xc2 && rd16(a + 1) == 0x14 &&
        rd8(a - 12) == 0xb8 && rd8(a - 7) == 0xba &&
        rd8(a - 2) == 0xff && rd8(a - 1) == 0xd2)
    {
        uint32_t esp = c->regs[ESP];
        c->eip = rd32( esp );
        c->regs[ESP] = esp + 4u + 0x14u;
        return 1;
    }
    /* Runtime callback veneers repeatedly use:
     *   mov eax,index; mov edx,veneer; call edx; ret 0x14
     * The veneer is an absolute-indirect jump to a small wasm dispatch index.
     * Preserve the call edge (including its return address) and enter that
     * ordinary dispatcher directly.  This is deliberately byte-pattern and
     * range guarded: arbitrary generated calls must remain interpreted. */
    if (rd8(a) == 0xb8 && rd8(a + 5) == 0xba && rd8(a + 10) == 0xff &&
        rd8(a + 11) == 0xd2 && rd8(a + 12) == 0xc2)
    {
        uint32_t veneer = rd32( a + 6 );
        if (veneer >= 0x30000000u && veneer < NAT_GUEST_END - 6u &&
            rd8(veneer) == 0xff && rd8(veneer + 1) == 0x25)
        {
            uint32_t slot = rd32( veneer + 2 );
            if (slot < NAT_GUEST_END - 4u)
            {
                uint32_t dispatch = rd32( slot ), esp = c->regs[ESP];
                if (dispatch && dispatch < 0x10000u && esp >= 4u)
                {
                    c->regs[EAX] = rd32( a + 1 );
                    c->regs[EDX] = veneer;
                    esp -= 4;
                    wr32( esp, a + 12u );
                    c->regs[ESP] = esp;
                    c->eip = dispatch;
                    return 1;
                }
            }
        }
    }
    if (rd8(a) == 0x8b && rd8(a + 1) == 0xff && rd8(a + 2) == 0x55 &&
        rd8(a + 3) == 0x8b && rd8(a + 4) == 0xec && rd8(a + 5) == 0x5d &&
        rd8(a + 6) == 0xff && rd8(a + 7) == 0x25)
    {
        /* mov edi,edi; push ebp; mov ebp,esp; pop ebp is net-zero. */
        p = a + 8;
    }
    else if (rd8(a) == 0xff && rd8(a + 1) == 0x25) p = a + 2;
    else if (rd8(a) == 0x5d && rd8(a + 1) == 0xff && rd8(a + 2) == 0x25)
    {
        uint32_t esp = c->regs[ESP];
        if (esp >= NAT_GUEST_END - 4u) return 0;
        c->regs[EBP] = rd32( esp );
        c->regs[ESP] = esp + 4;
        p = a + 3;
    }
    else if (rd8(a) == 0x8b && rd8(a + 1) == 0xec && rd8(a + 2) == 0x5d &&
             rd8(a + 3) == 0xff && rd8(a + 4) == 0x25)
    {
        uint32_t esp = c->regs[ESP];
        if (esp >= NAT_GUEST_END - 4u) return 0;
        c->regs[EBP] = rd32( esp );
        c->regs[ESP] = esp + 4;
        p = a + 5;
    }
    else return 0;
    p = rd32( p );
    if (p >= NAT_GUEST_END - 4u) return 0;
    c->eip = rd32( p );
    return 1;
}



/* RtlQueryPerformanceFrequency is a stdcall ntdll helper.  Wine's builtin
 * implementation returns the fixed 10 MHz performance-counter frequency;
 * keep the 64-bit store and callee-popped argument exactly as its guest body
 * does, while avoiding interpretation of the same tiny routine on every timer
 * query. */
static int nat_qpf( struct x86cpu *c )
{
    uint32_t esp = c->regs[ESP], p = rd32( esp + 4 );
    if (p >= NAT_GUEST_END - 8u) return 0;
    wr32( p, 10000000u ); wr32( p + 4, 0 );
    c->regs[EAX] = 1;
    c->eip = rd32( esp );
    c->regs[ESP] = esp + 8;       /* ret $4: return address + one argument */
    return 1;
}

/* RtlQueryPerformanceCounter is stdcall and returns the same monotonic clock
 * that the guest frequency helper describes.  emscripten_get_now() is
 * monotonic in the worker; converting milliseconds to 10 MHz ticks avoids the
 * nested NtQueryPerformanceCounter/syscall path for the renderer's frame
 * timing queries. */
static int nat_qpc( struct x86cpu *c )
{
    extern double emscripten_get_now( void );
    uint32_t esp = c->regs[ESP], p = rd32( esp + 4 );
    uint64_t ticks = (uint64_t)(emscripten_get_now() * 10000.0);
    if (p && p < NAT_GUEST_END - 8u) { wr32( p, (uint32_t)ticks ); wr32( p + 4, (uint32_t)(ticks >> 32) ); }
    c->regs[EAX] = 1;
    c->eip = rd32( esp );
    c->regs[ESP] = esp + 8;       /* ret $4 */
    return 1;
}

static int nat_gettid( struct x86cpu *c )
{
    uint32_t esp = c->regs[ESP], teb = c->fs_base;
    uint32_t self = teb ? rd32( teb + 0x18 ) : 0;
    c->regs[EAX] = self ? rd32( self + 0x24 ) : 0;
    c->eip = rd32( esp );
    c->regs[ESP] = esp + 4;
    return 1;
}

/* TlsGetValue is a one-argument stdcall helper.  Mirror kernelbase's exact
 * TEB lookup, including the expansion-slot limit and LastError update. */
static int nat_tlsgetvalue( struct x86cpu *c )
{
    uint32_t esp = c->regs[ESP], teb = c->fs_base, index = rd32( esp + 4 );
    uint32_t value = 0, slots;
    if (!teb || teb >= NAT_GUEST_END - 0xf98u) return 0;
    wr32( teb + 0x34, 0 ); /* SetLastError(ERROR_SUCCESS) */
    if (index < 64)
        value = rd32( teb + 0xe10u + index * 4u );
    else if (index < 64 + 256 && (slots = rd32( teb + 0xf94u )) != 0 &&
             slots < NAT_GUEST_END - 256u)
        value = rd32( slots + (index - 64u) * 4u );
    else if (index >= 64 + 256)
    {
        wr32( teb + 0x34, 87 ); /* ERROR_INVALID_PARAMETER */
    }
    c->regs[EAX] = value;
    c->eip = rd32( esp );
    c->regs[ESP] = esp + 8;       /* stdcall: return address + one argument */
    return 1;
}

static int nat_toascii( struct x86cpu *c )
{
    uint32_t esp = c->regs[ESP];
    return nat_ret_eax( c, esp, rd32( esp + 4 ) & 0x7f );
}

/* GCC's i386 __udivmoddi4 helper is cdecl and takes five stack words:
 * numerator low/high, denominator low/high, and an optional remainder
 * pointer.  The cdecl callee pops only the return address; its caller owns
 * the argument cleanup. */
static int nat_udivmoddi4( struct x86cpu *c )
{
    uint32_t esp = c->regs[ESP], rem = rd32( esp + 20 );
    uint64_t num = (uint64_t)rd32( esp + 4 ) | ((uint64_t)rd32( esp + 8 ) << 32);
    uint64_t den = (uint64_t)rd32( esp + 12 ) | ((uint64_t)rd32( esp + 16 ) << 32);
    uint64_t quotient, remainder;

    if (!den || (rem && rem >= NAT_GUEST_END - 8u)) return 0;
    quotient = num / den;
    remainder = num % den;
    if (rem) { wr32( rem, (uint32_t)remainder ); wr32( rem + 4, (uint32_t)(remainder >> 32) ); }
    c->regs[EAX] = (uint32_t)quotient;
    c->regs[EDX] = (uint32_t)(quotient >> 32);
    c->eip = rd32( esp );
    c->regs[ESP] = esp + 4;
    return 1;
}

/* MSVCRT's tolower/toupper exports have a locale-dependent slow path, but
 * their hot path is an exact unsigned-ASCII range check.  Only take that
 * path when the guest CRT's locale pointer is initialized (the same test the
 * guest code performs); otherwise return 0 so the interpreter retains the
 * full locale semantics. */
static int nat_casefold( struct x86cpu *c, int upper )
{
    uint32_t esp = c->regs[ESP];
    uint32_t base = 0x10000000u + (uint32_t)msvcrt_slide;
    uint32_t x = rd32( esp + 4 ), lo = upper ? 0x61u : 0x41u;
    if (!msvcrt_slide || !rd32( base + 0x7dfa8u )) return 0;
    if ((uint32_t)(x - lo) < 0x1au) x += upper ? (uint32_t)-0x20 : 0x20u;
    return nat_ret_eax( c, esp, x );
}

static int nat_memchr( struct x86cpu *c )
{
    uint32_t esp = c->regs[ESP], a = rd32( esp + 4 ), ch = rd32( esp + 8 );
    uint32_t n = rd32( esp + 12 ), i;
    if (n > NAT_GUEST_END || a > NAT_GUEST_END - n) return 0;
    for (i = 0; i < n; i++) if (rd8( a + i ) == (uint8_t)ch)
        return nat_ret_eax( c, esp, a + i );
    return nat_ret_eax( c, esp, 0 );
}

static int nat_strncmp( struct x86cpu *c )
{
    uint32_t esp = c->regs[ESP], a = rd32( esp + 4 ), b = rd32( esp + 8 );
    uint32_t n = rd32( esp + 12 ), i;
    if (n > NAT_GUEST_END || a > NAT_GUEST_END - n || b > NAT_GUEST_END - n) return 0;
    for (i = 0; i < n; i++)
    {
        uint8_t x = rd8( a + i ), y = rd8( b + i );
        if (x != y) return nat_ret_eax( c, esp, (uint32_t)(int)x - (uint32_t)(int)y );
        if (!x) break;
    }
    return nat_ret_eax( c, esp, 0 );
}

static int nat_strchr( struct x86cpu *c )
{
    uint32_t esp = c->regs[ESP], a = rd32( esp + 4 ), ch = rd32( esp + 8 ), p;
    if (a >= NAT_GUEST_END) return 0;
    for (p = a; p < NAT_GUEST_END; p++)
    {
        uint8_t x = rd8( p );
        if (x == (uint8_t)ch) return nat_ret_eax( c, esp, p );
        if (!x) break;
    }
    return nat_ret_eax( c, esp, 0 );
}

static int nat_strcpy( struct x86cpu *c )
{
    uint32_t esp = c->regs[ESP], dst = rd32( esp + 4 ), src = rd32( esp + 8 );
    uint32_t len = 0, i;
    if (dst >= NAT_GUEST_END || src >= NAT_GUEST_END) return 0;
    while (len < NAT_GUEST_END - src && rd8( src + len )) len++;
    if (len == NAT_GUEST_END - src || dst > NAT_GUEST_END - len - 1) return 0;
    for (i = 0; i <= len; i++) wr8( dst + i, rd8( src + i ));
    return nat_ret_eax( c, esp, dst );
}

static int nat_strncpy( struct x86cpu *c )
{
    uint32_t esp = c->regs[ESP], dst = rd32( esp + 4 ), src = rd32( esp + 8 );
    uint32_t n = rd32( esp + 12 ), i;
    if (n > NAT_GUEST_END || dst > NAT_GUEST_END - n || src > NAT_GUEST_END - n) return 0;
    for (i = 0; i < n; i++)
    {
        uint8_t x = rd8( src );
        wr8( dst + i, x );
        if (!x) { for (i++; i < n; i++) wr8( dst + i, 0); break; }
        src++;
    }
    return nat_ret_eax( c, esp, dst );
}

/* ---- mhlineskipmodify: the masked HORIZONTAL mapper ------------------------
 *
 * The floor/ceiling counterpart of mvlineasm4, and the last self-modifying
 * mapper of any size left.  A direct counter (not the sampler, which claimed
 * 7.2%) puts it at 1395 loop iterations a frame x 22 instructions = 4.5% of the
 * frame.
 *
 * Two pixels an iteration, from two texture coordinates: %ebp and %esi.  Each
 * pixel's texel index is built as ((coord >> shift) << n) | (esi >> (32 - n)) -
 * a `shr` immediately followed by `shld`, which is how Build glues the two
 * coordinates' high bits into one texture offset.  Every shift and base address
 * is patched per span, so all fourteen are read from the code on entry.
 *
 * 0xff is transparent, giving four cases per pair (both, neither, either), each
 * with its OWN patched shade-table address - mmach2d/2da/2db/6d are normally the
 * same table but are written separately, so they are read separately.
 *
 * Entering at mbeghline covers the odd-pixel prologue too, since that path ends
 * by jumping here.  The exit is easy: mendhline pops ebx/ecx/edx/esi/edi/ebp
 * straight back off the stack, so only %eax survives the function and only it
 * has to be right.
 */
#define ND_MHLINE      0x632a90u    /* mpreprebeghline: the template starts here */
#define ND_MHLINE_LEN  0x90u
#define ND_MHL_ENTRY   0x19u        /* mbeghline, where we intercept            */
#define ND_MHL_EXIT    0x90u        /* mendhline                                */

static const struct { uint16_t off; uint8_t len; } nd_mhline_imm[] = {
    { 0x04, 4 }, { 0x1b, 4 }, { 0x21, 1 }, { 0x25, 1 }, { 0x28, 4 }, { 0x2e, 4 },
    { 0x34, 4 }, { 0x3a, 1 }, { 0x3e, 1 }, { 0x41, 4 }, { 0x47, 4 }, { 0x59, 4 },
    { 0x61, 4 }, { 0x7e, 4 },
};
static const uint8_t nd_mhline_code[ND_MHLINE_LEN] = {
    0x88,0xc8,0x8a,0x80,0x00,0x00,0x00,0x00,0x88,0x07,0x83,0xc7,
    0x02,0x81,0xe9,0x00,0x00,0x02,0x00,0x0f,0x82,0x77,0x00,0x00,
    0x00,0x8d,0x9d,0x00,0x00,0x00,0x00,0xc1,0xed,0x00,0x0f,0xa4,
    0xf5,0x00,0x81,0xc6,0x00,0x00,0x00,0x00,0x8a,0x8d,0x00,0x00,
    0x00,0x00,0x8d,0xab,0x00,0x00,0x00,0x00,0xc1,0xeb,0x00,0x0f,
    0xa4,0xf3,0x00,0x81,0xc6,0x00,0x00,0x00,0x00,0x8a,0xab,0x00,
    0x00,0x00,0x00,0x80,0xf9,0xff,0x74,0x25,0x80,0xfd,0xff,0x74,
    0xab,0x88,0xc8,0x8a,0x98,0x00,0x00,0x00,0x00,0x88,0xe8,0x8a,
    0xb8,0x00,0x00,0x00,0x00,0x66,0x89,0x1f,0x83,0xc7,0x02,0x81,
    0xe9,0x00,0x00,0x02,0x00,0x73,0xa6,0xeb,0x1b,0x80,0xfd,0xff,
    0x74,0x90,0x88,0xe8,0x8a,0x80,0x00,0x00,0x00,0x00,0x88,0x47,
    0x01,0x83,0xc7,0x02,0x81,0xe9,0x00,0x00,0x02,0x00,0x73,0x89,
};

static int nd_mhline_skeleton_ok( uint32_t va )
{
    uint8_t wild[ND_MHLINE_LEN];
    unsigned i, k;

    memset( wild, 0, sizeof(wild) );
    for (i = 0; i < sizeof(nd_mhline_imm)/sizeof(nd_mhline_imm[0]); i++)
        for (k = 0; k < nd_mhline_imm[i].len; k++) wild[nd_mhline_imm[i].off + k] = 1;
    for (i = 0; i < ND_MHLINE_LEN; i++)
        if (!wild[i] && rd8( va + i ) != nd_mhline_code[i]) return 0;
    return 1;
}

static uint64_t g_mh_calls, g_mh_iters;
static int g_bt_left = 3;   /* WASM_BT: NULL-transfer backtraces before going quiet */

static int nat_mhlineskip( struct x86cpu *c )
{
    uint32_t t = ND_MHLINE + (uint32_t)nd_slide;
    uint32_t d2   = rd32( t + 0x04 ), mbeg = rd32( t + 0x1b );
    uint32_t s3   = rd8 ( t + 0x21 ) & 31, i4 = rd8( t + 0x25 ) & 31;
    uint32_t a4   = rd32( t + 0x28 ), d1   = rd32( t + 0x2e );
    uint32_t m7   = rd32( t + 0x34 );
    uint32_t s5   = rd8 ( t + 0x3a ) & 31, i6 = rd8( t + 0x3e ) & 31;
    uint32_t a8   = rd32( t + 0x41 ), d5   = rd32( t + 0x47 );
    uint32_t d2a  = rd32( t + 0x59 ), d2b  = rd32( t + 0x61 ), d6 = rd32( t + 0x7e );
    uint32_t eax = c->regs[EAX], ebx = c->regs[EBX], ecx = c->regs[ECX];
    uint32_t esi = c->regs[ESI], edi = c->regs[EDI], ebp = c->regs[EBP];
    unsigned n = 0;

    for (;;)
    {
        uint32_t cl, ch;

        ebx = ebp + mbeg;
        ebp >>= s3;  if (i4) ebp = (ebp << i4) | (esi >> (32 - i4));
        esi += a4;
        cl = rd8( ebp + d1 );
        ebp = ebx + m7;
        ebx >>= s5;  if (i6) ebx = (ebx << i6) | (esi >> (32 - i6));
        esi += a8;
        ch = rd8( ebx + d5 );

        if (cl != 0xff && ch != 0xff)
        {
            eax = ch;
            ebx = (ebx & 0xffff0000u) | rd8( cl + d2a ) | ((uint32_t)rd8( ch + d2b ) << 8);
            wr16( edi, (uint16_t)ebx );
        }
        else if (cl == 0xff && ch != 0xff)      /* mskip1 -> mmach6d */
        {
            eax = rd8( ch + d6 );
            wr8( edi + 1, (uint8_t)eax );
        }
        else if (cl != 0xff)                    /* ch transparent -> mmach2d */
        {
            eax = rd8( cl + d2 );
            wr8( edi, (uint8_t)eax );
        }
        /* else: both transparent, write nothing */

        edi += 2;
        n++;
        if (ecx < 0x20000u) { ecx -= 0x20000u; break; }   /* the borrow that ends it */
        ecx -= 0x20000u;
    }
    g_mh_calls++; g_mh_iters += n;

    /* mendhline pops ebx/ecx/edx/esi/edi/ebp, so only %eax outlives the call -
     * the rest are written back only to keep the visible state coherent. */
    c->regs[EAX] = eax; c->regs[EBX] = ebx; c->regs[ECX] = ecx;
    c->regs[ESI] = esi; c->regs[EDI] = edi; c->regs[EBP] = ebp;
    c->eip = t + ND_MHL_EXIT;
    return 1;
}

static void nat_arm_mhline( void )
{
    uint32_t t = ND_MHLINE + (uint32_t)nd_slide;
    if (nd_mhline_skeleton_ok( t )) nat_register( t + ND_MHL_ENTRY, NAT_MHLINE, "mhlineskipmodify" );
    else fprintf( stderr, "wasm_x86: mhlineskipmodify skeleton differs at %08x - left interpreted\n", t );
}

/* ---- there is no OpenGL here, so nothing is cached as "GL state is set" ----
 *
 * Changing the resolution from the in-game menu killed the session: the guest
 * called through a NULL function pointer and the interpreter ended the thread,
 * which on the page just looks like a freeze.  The log says why:
 *   "Failed loading OpenGL driver: ... OPENGL32.DLL: Module not found.;
 *    all OpenGL modes are unavailable."
 * Every GL entry point is NULL, and applying a video mode walks the engine's GL
 * state teardown even though the renderer is the software one.
 *
 * The engine caches "which GL capabilities are currently enabled" in a set of
 * inthash tables, and EVERY one of those teardown calls is gated on a lookup:
 *      if (inthash_find(cache, GL_BLEND)) { glDisable(GL_BLEND); ... }
 * There are ~100 such sites (212 references to the table array), so patching
 * call sites one at a time is not a fix.  Gating the lookup is: with no GL, no
 * capability can be enabled, so a lookup in THAT cache must miss, and every
 * dependent GL call is skipped by the guest's own branch.
 *
 * Deliberately narrow: only lookups in the 16 GL state tables are answered, and
 * only while the GL entry points really are NULL.  inthash_find is a general
 * utility used for other tables (sound, textures), and those must behave
 * normally - so anything else declines and runs the real function.
 */
#define ND_INTHASH_FIND 0x52a6a0u
#define ND_GLSTATE_TAB  0x00e167b8u   /* 16 x 16-byte inthash_t, one per texunit */
#define ND_GLSTATE_LEN  0x100u
#define ND_GLPROC_PROBE 0x00e16740u   /* one GL entry point: NULL => no GL at all */
static const uint8_t nd_inthash_code[] = {
    0x55,                       /* push %ebp        */
    0x89,0xe5,                  /* mov  %esp,%ebp   */
    0x57,                       /* push %edi        */
    0x89,0xc7,                  /* mov  %eax,%edi   (arg1: the table)  */
    0x56,                       /* push %esi        */
    0x89,0xd6,                  /* mov  %edx,%esi   (arg2: the key)    */
    0x53,                       /* push %ebx        */
    0x83,0xec,0x3c              /* sub  $0x3c,%esp  */
};

static int nat_inthash_find( struct x86cpu *c )
{
    uint32_t tab = c->regs[EAX];                     /* register args */
    uint32_t base = ND_GLSTATE_TAB + (uint32_t)nd_slide;
    uint32_t esp = c->regs[ESP];

    if (getenv( "WASM_NO_INTHASH_FAST" )) return 0;
    if (tab >= base && tab < base + ND_GLSTATE_LEN) {
        if (!rd32( ND_GLPROC_PROBE + (uint32_t)nd_slide )) {
            c->regs[EAX] = 0;                        /* no GL => miss */
            c->eip = rd32( esp ); c->regs[ESP] = esp + 4;
            return 1;
        }
        /* With a real GL context, continue through the exact generic lookup
         * below: the table contents, not the presence of GL, determine the
         * return value. */
    }

    /* General inthash_find is the same register-argument function used by
     * the GL cache.  The old shortcut deliberately declined this case, but
     * the resulting interpreter walk was a dominant post-frame hotspot.
     * Mirror the x86 body exactly: djb2 hash of the four key bytes, the
     * multiply-high reduction selected by the table's shift, then the
     * three-word bucket chain ([key], [value], [next]).  If the table is
     * uninitialized or any guest pointer is malformed, leave it interpreted
     * so its initialization/error path remains authoritative. */
    {
        uint32_t key = c->regs[EDX], buckets, count, shift8, shift;
        uint32_t hash = 0x1505u, i, node, value;
        uint64_t product;
        if (tab >= NAT_GUEST_END - 0x10u || esp >= NAT_GUEST_END - 4u) return 0;
        buckets = rd32( tab );
        if (!buckets || buckets >= NAT_GUEST_END - 12u) return 0;
        count = rd32( tab + 4 ); shift8 = rd8( tab + 0x0c ); shift = shift8 & 31u;
        for (i = 0; i < 4; i++) { hash = hash * 33u ^ ((key >> (i * 8)) & 0xffu); }
        if (rd32( tab + 8 )) {
            product = (uint64_t)rd32( tab + 8 ) * hash;
            if (shift8 & 0x40u) {
                uint32_t high = (uint32_t)(product >> 32);
                uint32_t delta = hash - high;
                i = (delta >> 1) + high;
                i >>= shift;
            } else i = (uint32_t)(product >> 32) >> shift;
        } else i = hash >> shift;
        i = (i * count);                         /* x86 imul, low 32 bits */
        i = hash - i;
        if (i > 0x55555555u || buckets + i * 12u >= NAT_GUEST_END) return 0;
        node = rd32( buckets + i * 12u );
        for (unsigned n = 0; node && n < (1u << 20); n++) {
            if (node >= NAT_GUEST_END - 12u) return 0;
            if (rd32( node ) == key) {
                value = rd32( node + 4 );
                c->regs[EAX] = value; c->eip = rd32( esp); c->regs[ESP] = esp + 4;
                return 1;
            }
            node = rd32( node + 8 );
        }
        if (node) return 0;                       /* malformed cycle */
        c->regs[EAX] = 0xffffffffu;               /* normal miss */
        c->eip = rd32( esp ); c->regs[ESP] = esp + 4;
        return 1;
    }
}

static void nat_arm_inthash( void )
{
    uint32_t b = ND_INTHASH_FIND + (uint32_t)nd_slide;
    unsigned i;
    for (i = 0; i < sizeof(nd_inthash_code); i++)
        if (rd8( b + i ) != nd_inthash_code[i])
        { fprintf( stderr, "wasm_x86: inthash_find differs at %08x - left alone\n", b ); return; }
    nat_register( b, NAT_GLSTATE, "GL state cache (no GL)" );
}

/* The sampler-object wrapper is NOT gated on the state cache above, so it needs
 * its own no-op: videoSetGameMode -> buildgl_resetSamplerObjects walks all 16
 * texture units and this wrapper's fallback branch (the one for hardware with no
 * sampler objects) sets the parameters on the bound texture directly.  With no
 * GL there is no bound texture and nothing to set.  Args arrive in registers,
 * so returning is just popping the return address. */
#define ND_GLSAMPLER      0x529500u
static const uint8_t nd_glsampler_code[] = {
    0x55,                       /* push %ebp                      */
    0x89,0xe5,                  /* mov  %esp,%ebp                 */
    0x83,0xec,0x18,             /* sub  $0x18,%esp                */
    0xf6,0x05,0,0,0,0,0x08,     /* testb $0x8,<glinfo flags>      */
    0x74,0x71                   /* je   <no-sampler-objects path> */
};

static int nat_glsampler( struct x86cpu *c )
{
    uint32_t esp = c->regs[ESP];
    if (rd32( ND_GLPROC_PROBE + (uint32_t)nd_slide )) return 0;   /* a real GL: run it */
    c->eip = rd32( esp );
    c->regs[ESP] = esp + 4;
    return 1;
}

static void nat_arm_glsampler( void )
{
    uint32_t b = ND_GLSAMPLER + (uint32_t)nd_slide;
    unsigned i;
    for (i = 0; i < sizeof(nd_glsampler_code); i++)
        if (i >= 8 && i <= 11) continue;                          /* the flags address */
        else if (rd8( b + i ) != nd_glsampler_code[i])
        { fprintf( stderr, "wasm_x86: buildgl_bindSamplerObject differs at %08x - left alone\n", b ); return; }
    nat_register( b, NAT_GLSAMPLER, "buildgl sampler (no GL)" );
}

/* ---- vlineasm1: the single-column texture mapper ---------------------------
 *
 * The last self-modifying mapper, and the simplest: one pixel per iteration in
 * eight instructions, with the texture and shade table already in registers so
 * only two fields are patched (the shift and the bytes-per-line step).
 *
 * A direct counter puts it at 1189 iterations a frame x 8 instructions = 1.4% of
 * the frame.  The sampler said 2.07%; the counter is the one to believe.
 *
 * Exit is easy: the epilogue pops ebp/edi/esi/edx/ecx/ebx, but it reads %edx
 * into %eax first (that is the return value), so %edx is the one register that
 * has to be right.
 */
#define ND_VLINE1     0x631cf7u    /* beginvline */
#define ND_VLINE1_LEN 0x1au
static const uint8_t nd_vline1_code[ND_VLINE1_LEN] = {
    0x89,0xd3,                  /* mov    %edx,%ebx              */
    0xc1,0xeb,0x00,             /* shr    $S,%ebx        (mach3a) */
    0x81,0xc7,0,0,0,0,          /* add    $BPL,%edi  (fixchain1b) */
    0x0f,0xb6,0x1c,0x1e,        /* movzbl (%esi,%ebx,1),%ebx     */
    0x01,0xc2,                  /* add    %eax,%edx              */
    0x49,                       /* dec    %ecx                   */
    0x8a,0x5c,0x1d,0x00,        /* mov    0x0(%ebp,%ebx,1),%bl   */
    0x88,0x1f,                  /* mov    %bl,(%edi)             */
    0x75,0xe6                   /* jne    beginvline             */
};

static uint64_t g_v1_calls, g_v1_iters;

static int nat_vlineasm1( struct x86cpu *c )
{
    uint32_t b   = ND_VLINE1 + (uint32_t)nd_slide;
    uint32_t entry = 0x631c90u + (uint32_t)nd_slide;
    uint32_t sh  = rd8 ( b + 0x04 ) & 31;
    uint32_t bpl = rd32( b + 0x07 );
    uint32_t eax = c->regs[EAX], ebx = c->regs[EBX], ecx = c->regs[ECX];
    uint32_t edx = c->regs[EDX], esi = c->regs[ESI], edi = c->regs[EDI], ebp = c->regs[EBP];
    unsigned n = 0;

    /* The true cdecl entry is 0x631c90, 0x67 bytes before beginvline.  The
     * runtime trace proves its frame: push ebx/ecx/edx/esi/edi, load six args,
     * test ECX, and on the nonzero path push EBP; the native loop's existing
     * epilogue then sees exactly [saved EBP, EDI, ESI, EDX, ECX, EBX, RET]. */
    if (c->eip == entry && !getenv( "WASM_NO_VLINE1_ENTRY" ))
    {
        uint32_t sp;
        c->regs[ESP] -= 4; wr32( c->regs[ESP], c->regs[EBX] );
        c->regs[ESP] -= 4; wr32( c->regs[ESP], c->regs[ECX] );
        c->regs[ESP] -= 4; wr32( c->regs[ESP], c->regs[EDX] );
        c->regs[ESP] -= 4; wr32( c->regs[ESP], c->regs[ESI] );
        c->regs[ESP] -= 4; wr32( c->regs[ESP], c->regs[EDI] );
        sp = c->regs[ESP];
        c->regs[EAX] = rd32( sp + 0x18 );
        c->regs[EBX] = rd32( sp + 0x1c );
        c->regs[ECX] = rd32( sp + 0x20 );
        c->regs[EDX] = rd32( sp + 0x24 );
        c->regs[ESI] = rd32( sp + 0x28 );
        c->regs[EDI] = rd32( sp + 0x2c );
        if (!c->regs[ECX])
        {
            c->regs[EBX] = rd32( sp ); c->regs[ECX] = rd32( sp + 4 );
            c->regs[EDX] = rd32( sp + 8 ); c->regs[ESI] = rd32( sp + 12 );
            c->regs[EDI] = rd32( sp + 16 ); c->eip = rd32( sp + 20 );
            c->regs[ESP] = sp + 24;
            return 1;
        }
        c->regs[ESP] -= 4; wr32( c->regs[ESP], c->regs[EBP] );
        c->regs[EBP] = c->regs[EBX];
        c->regs[ECX]++;
        eax = c->regs[EAX]; ebx = c->regs[EBX]; ecx = c->regs[ECX];
        edx = c->regs[EDX]; esi = c->regs[ESI]; edi = c->regs[EDI];
        ebp = c->regs[EBP];
    }

    if (!ecx) return 0;      /* would wrap to 4G iterations - let the guest have it */

    do {
        ebx = edx >> sh;
        edi += bpl;
        ebx = rd8( esi + ebx );          /* texel */
        edx += eax;                      /* texture position += step */
        ecx--;
        ebx = rd8( ebp + ebx );          /* shade; only %bl is written, and the
                                          * movzbl above left the top bytes zero */
        wr8( edi, (uint8_t)ebx );
        n++;
    } while (ecx);
    g_v1_calls++; g_v1_iters += n;

    /* Complete the same epilogue as mvlineasm1 below.  The final instruction
     * in this loop is `dec ecx`, so use K_DEC (which preserves CF) rather than
     * modeling it as a subtraction. */
    set_lazy(c,K_DEC,1,1,0,4);
    { uint32_t sp=c->regs[ESP], result=edx;
      c->regs[EBP]=rd32(sp); c->regs[EDI]=rd32(sp+4); c->regs[ESI]=rd32(sp+8);
      c->regs[EDX]=rd32(sp+12); c->regs[ECX]=rd32(sp+16); c->regs[EBX]=rd32(sp+20);
      c->regs[ESP]=sp+24; c->regs[EAX]=result; c->eip=rd32(sp+24); }
    return 1;
}

static void nat_arm_vline1( void )
{
    uint32_t b = ND_VLINE1 + (uint32_t)nd_slide;
    uint32_t entry = 0x631c90u + (uint32_t)nd_slide;
    unsigned i;
    for (i = 0; i < ND_VLINE1_LEN; i++)
        if (i == 0x04 || (i >= 0x07 && i <= 0x0a)) continue;   /* the patched fields */
        else if (rd8( b + i ) != nd_vline1_code[i])
        { fprintf( stderr, "wasm_x86: vlineasm1 skeleton differs at %08x - left interpreted\n", b ); return; }
    for (i = 0; i < 29; i++)
        if (rd8( entry + i ) != (const uint8_t[]){
                0x53,0x51,0x52,0x56,0x57,0x8b,0x44,0x24,0x18,
                0x8b,0x5c,0x24,0x1c,0x8b,0x4c,0x24,0x20,
                0x8b,0x54,0x24,0x24,0x8b,0x74,0x24,0x28,
                0x8b,0x7c,0x24,0x2c,0x85}[i])
            { fprintf( stderr, "wasm_x86: vlineasm1 entry skeleton differs at %08x - loop only\n", entry + i ); goto loop_only; }
    if (rd8( entry + 29 ) != 0x85 || rd8( entry + 30 ) != 0xc9 ||
        rd8( entry + 31 ) != 0x75 || rd8( entry + 32 ) != 0x3c ||
        rd8( 0x631cedu + (uint32_t)nd_slide ) != 0x55 ||
        rd8( 0x631ceeu + (uint32_t)nd_slide ) != 0x89 ||
        rd8( 0x631cefu + (uint32_t)nd_slide ) != 0xdd)
        { fprintf( stderr, "wasm_x86: vlineasm1 entry branch skeleton differs - loop only\n" ); goto loop_only; }
    if (!getenv( "WASM_NO_VLINE1_ENTRY" ))
        nat_register( entry, NAT_VLINE1, "vlineasm1 entry" );
loop_only:
    nat_register( b, NAT_VLINE1, "vlineasm1" );
}

/* ---- mvlineasm1: the masked single-column mapper --------------------------
 * Same fixed-point walk as vlineasm1, but transparent texels (0xff) leave the
 * destination untouched.  The two immediates are patched by the engine, so the
 * skeleton check deliberately wildcards only those fields. */
#define ND_MVLINE1 0x631db0u
#define ND_MVLINE1_LEN 0x2au
static const uint8_t nd_mvline1_code[ND_MVLINE1_LEN] = {
    0x89,0xd3, 0xc1,0xeb,0, 0x0f,0xb6,0x1c,0x1e, 0x80,0xfb,0xff,
    0x74,0x06, 0x8a,0x5c,0x1d,0, 0x88,0x1f, 0x01,0xc2,
    0x81,0xc7,0,0,0,0, 0x83,0xe9,0x01, 0x73,0xdf,
    0x5d,0x89,0xd0, 0x5f,0x5e,0x5a,0x59,0x5b,0xc3
};
static uint64_t g_mv1_calls, g_mv1_iters;
static int nat_mvlineasm1( struct x86cpu *c )
{
    uint32_t b=ND_MVLINE1+(uint32_t)nd_slide, sh=rd8(b+4)&31, bpl=rd32(b+0x18);
    uint32_t eax=c->regs[EAX], ebx=c->regs[EBX], ecx=c->regs[ECX];
    uint32_t edx=c->regs[EDX], esi=c->regs[ESI], edi=c->regs[EDI], ebp=c->regs[EBP];
    uint64_t n, total;

    /* The public entry is five pushes plus six argument loads before the
     * verified loop address.  The profile shows those pushes are hot because
     * the old hook started at beginmvline.  Recreate the exact frame when the
     * hook is armed at the true entry, then run the same loop/epilogue below. */
    if (c->eip == b - 0x20u)
    {
        uint32_t sp;
        c->regs[ESP] -= 4; wr32( c->regs[ESP], c->regs[EBX] );
        c->regs[ESP] -= 4; wr32( c->regs[ESP], c->regs[ECX] );
        c->regs[ESP] -= 4; wr32( c->regs[ESP], c->regs[EDX] );
        c->regs[ESP] -= 4; wr32( c->regs[ESP], c->regs[ESI] );
        c->regs[ESP] -= 4; wr32( c->regs[ESP], c->regs[EDI] );
        sp = c->regs[ESP];
        c->regs[EAX] = rd32( sp + 0x18 );
        c->regs[EBX] = rd32( sp + 0x1c );
        c->regs[ECX] = rd32( sp + 0x20 );
        c->regs[EDX] = rd32( sp + 0x24 );
        c->regs[ESI] = rd32( sp + 0x28 );
        c->regs[EDI] = rd32( sp + 0x2c );
        c->regs[ESP] -= 4; wr32( c->regs[ESP], c->regs[EBP] );
        c->regs[EBP] = c->regs[EBX];
        eax=c->regs[EAX]; ebx=c->regs[EBX]; ecx=c->regs[ECX];
        edx=c->regs[EDX]; esi=c->regs[ESI]; edi=c->regs[EDI]; ebp=c->regs[EBP];
        n=(uint64_t)ecx+1; total=n; if(n>(1u<<24)) return 0;
    }
    else
    {
        n=(uint64_t)ecx+1; total=n; if(n>(1u<<24)) return 0;
    }
    while(n--){ ebx=rd8(esi+(edx>>sh)); if((uint8_t)ebx!=0xff){ ebx=rd8(ebp+ebx); wr8(edi,(uint8_t)ebx); } edx+=eax; edi+=bpl; ecx--; }
    g_mv1_calls++; g_mv1_iters += total;
    /* Complete the fixed epilogue here.  The loop's final `sub ecx,1` leaves
     * lazy subtraction flags live, while the epilogue restores every callee-
     * saved register and returns with EAX=EDX from the loop.  Doing this at
     * the native boundary removes six interpreted pops plus the ret on every
     * single-column call; the skeleton still covers the original epilogue so
     * a patched build cannot silently move this contract. */
    if (total) set_lazy(c,K_SUB,0,1,0xffffffffu,4);
    { uint32_t sp=c->regs[ESP], result=edx;
      c->regs[EBP]=rd32(sp); c->regs[EDI]=rd32(sp+4); c->regs[ESI]=rd32(sp+8);
      c->regs[EDX]=rd32(sp+12); c->regs[ECX]=rd32(sp+16); c->regs[EBX]=rd32(sp+20);
      c->regs[ESP]=sp+24; c->regs[EAX]=result; c->eip=rd32(sp+24); }
    return 1;
}
static void nat_arm_mvline1( void )
{
    uint32_t b=ND_MVLINE1+(uint32_t)nd_slide; unsigned i;
    for(i=0;i<ND_MVLINE1_LEN;i++)
        if(i==4 || (i>=22 && i<=25)) continue;
        else if(rd8(b+i)!=nd_mvline1_code[i]){ fprintf(stderr,"wasm_x86: mvlineasm1 skeleton differs at %08x - left interpreted\n",b+i); return; }
    if (!getenv( "WASM_NO_MVLINE1_ENTRY" ))
        nat_register(b - 0x20u,NAT_MVLINE1,"mvlineasm1");
}

/* ---- non-power-of-two single-column mappers (opt-in) ----------------------
 *
 * These two SMC loops are structurally similar to vlineasm1, but their
 * texture coordinate is the high half of an unsigned MUL.  The replacement
 * must carry the complete architectural result of MUL and of the final
 * texel/palette load: EAX and EDX are live at the epilogue, not just the
 * pointer registers.  Keep this behind an explicit experiment flag until a
 * longer differential run has covered all callers.
 */
#define ND_VLINE1NP2  0x631d59u
#define ND_MVLINE1NP2 0x631e12u
static uint64_t g_v1n_calls, g_v1n_iters, g_mv1n_calls, g_mv1n_iters;
static int g_np2_trace_done;
static int nat_line1np2( struct x86cpu *c, int masked )
{
    uint32_t b = (masked ? ND_MVLINE1NP2 : ND_VLINE1NP2) + (uint32_t)nd_slide;
    if (c->eip == b - (masked ? 0x32u : 0x39u))
    {
        uint32_t sp;
        /* Both NP2 entry points have the same cdecl six-argument prologue.
         * Recreate its five saved registers and EBP frame, then load the
         * arguments exactly as the guest does before entering the SMC loop. */
        c->regs[ESP] -= 4; wr32( c->regs[ESP], c->regs[EBX] );
        c->regs[ESP] -= 4; wr32( c->regs[ESP], c->regs[ECX] );
        c->regs[ESP] -= 4; wr32( c->regs[ESP], c->regs[EDX] );
        c->regs[ESP] -= 4; wr32( c->regs[ESP], c->regs[ESI] );
        c->regs[ESP] -= 4; wr32( c->regs[ESP], c->regs[EDI] );
        sp = c->regs[ESP];
        c->regs[EAX] = rd32( sp + 0x18 );
        c->regs[EBX] = rd32( sp + 0x1c );
        c->regs[ECX] = rd32( sp + 0x20 ) + 1u;
        c->regs[EDX] = rd32( sp + 0x24 );
        c->regs[ESI] = rd32( sp + 0x28 );
        c->regs[EDI] = rd32( sp + 0x2c );
        wr32( b + (masked ? 0x15u : 0x1au), c->regs[EBX] );
        c->regs[ESP] -= 4; wr32( c->regs[ESP], c->regs[EBP] );
        c->regs[EBP] = c->regs[EDX];
    }
    uint32_t step = c->regs[EBX], tex = c->regs[ESI], dst = c->regs[EDI];
    uint32_t pos = c->regs[EBP], count = c->regs[ECX];
    uint32_t mul = rd32(b + 1), stride = rd32(b + (masked ? 0x1f : 0x09));
    uint32_t pal = rd32(b + (masked ? 0x15 : 0x1a));
    uint64_t n = (uint64_t)count + 1, total = n;
    if (getenv("WASM_NP2_TRACE")) {
        if (!g_np2_trace_done) {
            fprintf(stderr, "NP2TRACE eip=%08x ecx=%08x ebp=%08x ebx=%08x esi=%08x edi=%08x mul=%08x stride=%08x pal=%08x\n",
                    b, count, pos, step, tex, dst, mul, stride, pal);
            g_np2_trace_done = 1;
        }
        return 0;
    }
    if (!count || n > (1u << 24)) return 0;
    while (n--) {
        uint64_t product = (uint64_t)mul * pos;
        uint32_t low = (uint32_t)product, high = (uint32_t)(product >> 32);
        uint32_t write_dst = masked ? dst : dst + stride;
        if ((uint64_t)tex + high > 0xffffffffu ||
            (!masked && write_dst < dst) ||
            (uint64_t)pal + 0xffu > 0xffffffffu ||
            (uint64_t)write_dst > 0xffffffffu)
            return 0;
        uint8_t v;
        c->regs[EAX] = low & 0xffu;
        c->regs[EDX] = high;
        v = rd8(tex + high);
        c->regs[EAX] = v;
        if (masked) {
            if (v != 0xff) { v = rd8(pal + v); c->regs[EAX] = v; wr8(dst, v); }
            pos += step; dst += stride;
            c->regs[EBP] = pos; c->regs[EDI] = dst;
            { uint32_t old = count; count--; set_lazy(c, K_SUB, old, 1, count, 4); }
        } else {
            dst = write_dst;
            v = rd8(pal + v); c->regs[EAX] = v; wr8(dst, v);
            pos += step;
            c->regs[EBP] = pos; c->regs[EDI] = dst;
            { uint32_t old = count; count--; set_lazy(c, K_DEC, old, 1, count, 4); }
        }
    }
    c->regs[EBX] = step; c->regs[ECX] = count; c->regs[ESI] = tex;
    c->regs[EBP] = pos; c->regs[EDI] = dst;
    c->eip = b + (masked ? 0x28 : 0x22);
    if (masked) { g_mv1n_calls++; g_mv1n_iters += total; }
    else { g_v1n_calls++; g_v1n_iters += total; }
    return 1;
}
static int nat_vlineasm1np2( struct x86cpu *c ) { return nat_line1np2(c, 0); }
static int nat_mvlineasm1np2( struct x86cpu *c ) { return nat_line1np2(c, 1); }
static void nat_arm_line1np2( void )
{
    uint32_t v = ND_VLINE1NP2 + (uint32_t)nd_slide, m = ND_MVLINE1NP2 + (uint32_t)nd_slide;
    int vok = rd8(v) == 0xb8 && rd8(v+7) == 0x81 && rd8(v+0x18) == 0x8a;
    int mok = rd8(m) == 0xb8 && rd8(m+0x1d) == 0x81 && rd8(m+0x13) == 0x8a;
    /* Check the fixed prologue opcodes at the true entry; the two absolute
     * SMC immediates are deliberately checked only by their instruction
     * opcodes, since their values are rewritten by each call. */
    if (vok && (rd8(v-0x39u+0x1d)!=0x89 || rd8(v-0x39u+0x1e)!=0x1d ||
                rd8(v-0x39u+0x23)!=0x55 || rd8(v-0x39u+0x24)!=0x89 ||
                rd8(v-0x39u+0x25)!=0xd5 || rd8(v-0x39u+0x28)!=0xa1 ||
                rd8(v-0x39u+0x2d)!=0xa3)) vok = 0;
    if (mok && (rd8(m-0x32u+0x1d)!=0x89 || rd8(m-0x32u+0x1e)!=0x1d ||
                rd8(m-0x32u+0x23)!=0x55 || rd8(m-0x32u+0x24)!=0x89 ||
                rd8(m-0x32u+0x25)!=0xd5 || rd8(m-0x32u+0x28)!=0xa1 ||
                rd8(m-0x32u+0x2d)!=0xa3)) mok = 0;
    if (vok) nat_register(v-0x39u, NAT_VLINE1NP2, "vlineasm1nonpow2");
    if (mok) nat_register(m-0x32u, NAT_MVLINE1NP2, "mvlineasm1nonpow2");
}

/* ---- Bcrc32: the engine's CRC32, native ---------------------------------
 *
 * At level load EDuke32 CRC32-hashes texture data to key its GL texture cache;
 * the guest CRC loop (crc32.cpp, Bcrc32) then dominates wall-time under the
 * interpreter and the level can take minutes to load, looking like a hang.
 * Bcrc32 is a slice-by-four CRC32:
 *   uint32_t Bcrc32( const void *buf, int len, uint32_t crc )   // -mregparm=3
 * so buf is in eax, len in edx, crc in ecx, and it inverts crc on both ends.
 * The four 256-entry tables live in guest data at fixed addresses; read them
 * straight from there so the result is bit-identical to the interpreted loop
 * whatever polynomial they hold.  Skeleton-verified before arming. */
#define ND_CRC32      0x4e3070u
#define ND_CRC_T0     0x12247c0u   /* indexed by the top byte    */
#define ND_CRC_T1     0x1224bc0u   /* indexed by byte 2          */
#define ND_CRC_T2     0x1224fc0u   /* indexed by byte 1          */
#define ND_CRC_T3     0x12253c0u   /* indexed by the bottom byte */
static const uint8_t nd_crc32_code[] = {
    0x55,                   /* push %ebp        */
    0xf7,0xd1,              /* not  %ecx        */
    0x89,0xe5,              /* mov  %esp,%ebp   */
    0x57,                   /* push %edi        */
    0x89,0xd7,              /* mov  %edx,%edi   */
    0x89,0xca,              /* mov  %ecx,%edx   */
    0x56,                   /* push %esi        */
    0x53,                   /* push %ebx        */
    0x89,0xc3,              /* mov  %eax,%ebx   */
    0x83,0xec,0x08          /* sub  $0x8,%esp   */
};

static int nat_crc32( struct x86cpu *c )
{
    uint32_t buf = c->regs[EAX];
    int32_t  len = (int32_t)c->regs[EDX];
    uint32_t crc = ~c->regs[ECX];
    uint32_t t0 = ND_CRC_T0 + (uint32_t)nd_slide, t1 = ND_CRC_T1 + (uint32_t)nd_slide;
    uint32_t t2 = ND_CRC_T2 + (uint32_t)nd_slide, t3 = ND_CRC_T3 + (uint32_t)nd_slide;
    uint32_t esp = c->regs[ESP];

    while (len >= 4)
    {
        uint32_t w = crc ^ rd32( buf );
        buf += 4; len -= 4;
        crc = rd32( t0 + (((w >> 24) & 0xff) << 2) ) ^ rd32( t1 + (((w >> 16) & 0xff) << 2) )
            ^ rd32( t2 + (((w >>  8) & 0xff) << 2) ) ^ rd32( t3 + ((w & 0xff) << 2) );
    }
    while (len-- > 0)
    {
        crc = (crc >> 8) ^ rd32( t0 + (((crc ^ rd8( buf )) & 0xff) << 2) );
        buf++;
    }
    c->regs[EAX] = ~crc;
    c->eip = rd32( esp );
    c->regs[ESP] = esp + 4;
    return 1;
}

static void nat_arm_crc32( void )
{
    uint32_t b = ND_CRC32 + (uint32_t)nd_slide;
    unsigned i;
    for (i = 0; i < sizeof(nd_crc32_code); i++)
        if (rd8( b + i ) != nd_crc32_code[i])
        { fprintf( stderr, "wasm_x86: Bcrc32 skeleton differs at %08x - left interpreted\n", b ); return; }
    nat_register( b, NAT_CRC32, "Bcrc32" );
}

/* paletteGetClosestColorWithBlacklist (colmatch.cpp) - the grid-accelerated
 * nearest-palette-colour search the engine runs tens of thousands of times
 * while building shade/palswap lookup tables at first level load.  Under the
 * interpreter its inner cache scan (up to 4096 entries) plus grid walk dominate
 * wall-time and make the first load take minutes (STALLBT catches it inside
 * this function).  It is -mregparm=3, so r/g/b arrive in eax/edx/ecx and
 * lastokcol/blacklist on the stack; it returns in eax with a plain ret.
 *
 * The function is a memoising wrapper around a pure grid search: every value it
 * ever returns is a fresh grid-search result (the cache is flushed whenever
 * lastokcol or blacklist changes, so a hit can only echo a miss under the same
 * arguments).  So computing the grid search directly - ignoring the cache
 * entirely - yields bit-identical returns without touching any guest state,
 * exactly like the crc32 hook.  All the tables live at fixed guest addresses;
 * every byte the compiler loads from them is movzx, i.e. unsigned char, which
 * this mirrors.  FASTPALGRIDSIZ is 256>>3 = 32. */
#define ND_PALMATCH   0x4e28a0u
#define ND_RDIST      0xf8e9e0u    /* int32_t[513], indexed by pal.r + (256-r) */
#define ND_GDIST      0xf8e1c0u    /* int32_t[513]                             */
#define ND_BDIST      0xf8d9a0u    /* int32_t[513]                             */
#define ND_COLDIST    0xf8f1e8u    /* uint8_t[8]                               */
#define ND_COLSCAN    0xf8f200u    /* int32_t[27], grid-cell offsets           */
#define ND_COLHEAD    0x1225be0u   /* uint8_t[], per-cell linked-list head     */
#define ND_COLHERE    0x122f580u   /* bitmap, cell occupied                    */
#define ND_COLNEXT    0x12257c0u   /* int32_t[256], linked-list next, -1 ends  */
#define ND_COLPAL     0x1225bc0u   /* uint8_t[768], the matched palette        */
#define ND_PALETTE    0xf8f280u    /* uint8_t[768], the engine's base palette  */
#define ND_NUMCOLRES2 0x12348c0u   /* int32_t numcolmatchresults (result cache)*/
#define ND_GRIDCELLS  ((32+2)*(32+2)*(32+2))

/* Re-sync colmatch_palette + its lookup grid to the engine's base `palette`,
 * replicating paletteInitClosestColorMap (colmatch.cpp).  The engine is supposed to
 * keep colmatch_palette == the base palette (both paletteInitClosestColorMap callers
 * pass `palette`), but on this build it can be left holding a stale palette while
 * `palette` already holds the correct base one - so paletteGetClosestColor matches the
 * wrong palette and paletteMakeLookupTable builds wrong (e.g. all-black) tables.  The
 * interpreted wrapper hid this via its result cache; this native hook recomputes fresh,
 * so it needs colmatch_palette correct.  Restores the invariant without touching speed. */
static void nd_pal_resync( uint32_t s )
{
    uint32_t pal = ND_PALETTE + s, cmp = ND_COLPAL + s;
    uint32_t colhere = ND_COLHERE + s, colhead = ND_COLHEAD + s, colnext = ND_COLNEXT + s;
    unsigned i, nbytes = (ND_GRIDCELLS + 7) >> 3;
    for (i = 0; i < 768; i++) wr8( cmp + i, rd8( pal + i ) );
    for (i = 0; i + 4 <= nbytes; i += 4) wr32( colhere + i, 0 );
    for (; i < nbytes; i++) wr8( colhere + i, 0 );
    for (i = 256; i-- > 0; )
    {
        uint32_t p = cmp + i * 3, hb;
        int j = (rd8( p ) >> 3) * 32 * 32 + (rd8( p + 1 ) >> 3) * 32 + (rd8( p + 2 ) >> 3)
                + 32 * 32 + 32 + 1;
        hb = colhere + ((uint32_t)j >> 3);
        if (rd8( hb ) & (1u << (j & 7))) wr32( colnext + i * 4, rd8( colhead + (uint32_t)j ) );
        else                             wr32( colnext + i * 4, 0xffffffffu );
        wr8( colhead + (uint32_t)j, (uint8_t)i );
        wr8( hb, rd8( hb ) | (1u << (j & 7)) );
    }
    wr32( ND_NUMCOLRES2 + s, 0 );
}

static const uint8_t nd_palmatch_code[] = {
    0x55,                   /* push %ebp        */
    0x89,0xe5,              /* mov  %esp,%ebp   */
    0x57,                   /* push %edi        */
    0x56,                   /* push %esi        */
    0x89,0xc6,              /* mov  %eax,%esi   */
    0x89,0xc8,              /* mov  %ecx,%eax   */
    0x53,                   /* push %ebx        */
    0xc1,0xe0,0x10,         /* shl  $0x10,%eax  */
    0x09,0xf0,              /* or   %esi,%eax   */
    0x83,0xec,0x2c          /* sub  $0x2c,%esp  */
};

static int nat_pal_closest( struct x86cpu *c )
{
    int32_t  r = (int32_t)c->regs[EAX];
    int32_t  g = (int32_t)c->regs[EDX];
    int32_t  b = (int32_t)c->regs[ECX];
    int32_t  lastokcol = (int32_t)garg( c, 0 );
    uint32_t blacklist = garg( c, 1 );
    uint32_t s = (uint32_t)nd_slide;
    uint32_t rdist = ND_RDIST + s, gdist = ND_GDIST + s, bdist = ND_BDIST + s;
    uint32_t coldist = ND_COLDIST + s, colscan = ND_COLSCAN + s;
    uint32_t colhead = ND_COLHEAD + s, colhere = ND_COLHERE + s;
    uint32_t colnext = ND_COLNEXT + s, colpal = ND_COLPAL + s;
    const int GRID = 32;

    {
        static const unsigned sent[4] = { 31 * 3, 96 * 3, 160 * 3, 224 * 3 };
        unsigned kk;
        for (kk = 0; kk < 4; kk++)
            if (rd8( colpal + sent[kk] ) != rd8( ND_PALETTE + s + sent[kk] ) ||
                rd8( colpal + sent[kk] + 2 ) != rd8( ND_PALETTE + s + sent[kk] + 2 ))
            { nd_pal_resync( s ); break; }
    }

    int j = (r >> 3) * GRID * GRID + (g >> 3) * GRID + (b >> 3) + GRID * GRID + GRID + 1;
    int minrdist = (int32_t)rd32( rdist + (uint32_t)(rd8( coldist + (r & 7) ) + 256) * 4 );
    int mingdist = (int32_t)rd32( gdist + (uint32_t)(rd8( coldist + (g & 7) ) + 256) * 4 );
    int minbdist = (int32_t)rd32( bdist + (uint32_t)(rd8( coldist + (b & 7) ) + 256) * 4 );
    int mindist = minrdist < mingdist ? minrdist : mingdist;
    if (minbdist < mindist) mindist = minbdist;
    mindist += 1;

    int R = 256 - r, G = 256 - g, B = 256 - b;
    int retcol = -1, k;

    for (k = 26; k >= 0; k--)
    {
        int i = (int32_t)rd32( colscan + (uint32_t)k * 4 ) + j;
        if (!(rd8( colhere + ((uint32_t)i >> 3) ) & (1u << (i & 7)))) continue;
        i = rd8( colhead + (uint32_t)i );
        do {
            uint32_t pal1 = colpal + (uint32_t)i * 3;
            int dist = (int32_t)rd32( gdist + (uint32_t)(rd8( pal1 + 1 ) + G) * 4 );
            if (!(dist >= mindist || i > lastokcol ||
                  (blacklist && (rd8( blacklist + ((uint32_t)i >> 3) ) & (1u << (i & 7))))))
            {
                if ((dist += (int32_t)rd32( rdist + (uint32_t)(rd8( pal1 ) + R) * 4 )) < mindist &&
                    (dist += (int32_t)rd32( bdist + (uint32_t)(rd8( pal1 + 2 ) + B) * 4 )) < mindist)
                { mindist = dist; retcol = i; }
            }
            i = (int32_t)rd32( colnext + (uint32_t)i * 4 );
        } while (i >= 0);
    }

    if (retcol < 0)
    {
        int i;
        mindist = 0x7fffffff;
        for (i = 0; i <= lastokcol; i++)
        {
            uint32_t pal1;
            int dist;
            if (blacklist && (rd8( blacklist + ((uint32_t)i >> 3) ) & (1u << (i & 7)))) continue;
            pal1 = colpal + (uint32_t)i * 3;
            dist = (int32_t)rd32( gdist + (uint32_t)(rd8( pal1 + 1 ) + G) * 4 );
            if (dist >= mindist) continue;
            if ((dist += (int32_t)rd32( rdist + (uint32_t)(rd8( pal1 ) + R) * 4 )) >= mindist) continue;
            if ((dist += (int32_t)rd32( bdist + (uint32_t)(rd8( pal1 + 2 ) + B) * 4 )) >= mindist) continue;
            mindist = dist; retcol = i;
        }
    }

    {
        uint32_t esp = c->regs[ESP];
        c->regs[EAX] = (uint32_t)retcol;
        c->eip = rd32( esp );
        c->regs[ESP] = esp + 4;
    }
    return 1;
}

static void nat_arm_pal_closest( void )
{
    uint32_t b = ND_PALMATCH + (uint32_t)nd_slide;
    unsigned i;
    for (i = 0; i < sizeof(nd_palmatch_code); i++)
        if (rd8( b + i ) != nd_palmatch_code[i])
        { fprintf( stderr, "wasm_x86: palmatch skeleton differs at %08x - left interpreted\n", b ); return; }
    nat_register( b, NAT_PALMATCH, "paletteGetClosestColorWithBlacklist" );
}

/* MV_MixStereo<uint8_t,int16_t> (audiolib mix.cpp) - the SFX software mixer's
 * hot per-sample loop.  In gameplay the audio callback runs on this same single
 * interpreter thread and this mix is its biggest slice.  Reimplement the loop
 * bit-exactly: 8-bit source flipped to signed and shifted up, two
 * fix16_fast_trunc_mul stages (global*panned volume, then *sample), clamp to
 * int16, interleaved-stereo store, and the per-sample volume smoothing that the
 * guest writes back into voice->PannedVolume.  cdecl(voice, length); returns the
 * advanced sample position in EAX.  All volumes/rate/globals are read from the
 * same places the guest reads them, so the mixed output is identical whatever
 * they hold (checked against an offline reference over 20k random cases).
 * Skeleton-verified before arming; -1 entries below skip the one relocated
 * imm32 (the mov MV_MixDestination,%edi). */
#define ND_MIXSTEREO   0x594550u
#define ND_MV_MIXDEST  0x01ab7b68u   /* MV_MixDestination     (int16* as a VA) */
#define ND_MV_GLOBVOL  0x00826eecu   /* MV_GlobalVolume       (fix16) */
#define ND_MV_SMOOTH   0x0082c40cu   /* MV_VolumeSmoothFactor (fix16) */
#define ND_MV_RIGHTOFF 0x01ab7c40u   /* MV_RightChannelOffset (bytes) */
static const int16_t nd_mixstereo_code[] = {
    0x55, 0x89,0xe5, 0x57, 0x56, 0x53, 0x83,0xec,0x34, /* push ebp;mov esp,ebp;push edi,esi,ebx;sub 0x34,esp */
    0x8b,0x75,0x08,                                     /* mov 0x8(ebp),esi   (voice)              */
    0x8b,0x3d, -1,-1,-1,-1,                             /* mov MV_MixDestination,edi   (relocated) */
    0x8b,0x46,0x10,                                     /* mov 0x10(esi),eax  (voice->sound)       */
    0x8b,0x5e,0x6c,                                     /* mov 0x6c(esi),ebx  (voice->position)    */
    0x89,0x7d,0xc8,                                     /* mov edi,-0x38(ebp)                      */
    0x8b,0x4e,0x30                                      /* mov 0x30(esi),ecx  (PannedVolume.Left)  */
};

static int nat_mixstereo( struct x86cpu *c )
{
    uint32_t voice  = garg( c, 0 );
    uint32_t length = garg( c, 1 );
    uint32_t esp    = c->regs[ESP];

    uint32_t sound  = rd32( voice + 0x10 );
    uint32_t pos    = rd32( voice + 0x6c );
    uint32_t rate   = rd32( voice + 0x68 );
    int32_t  gvol   = (int32_t)rd32( ND_MV_GLOBVOL + (uint32_t)nd_slide );
    int32_t  volume = (int32_t)(((int64_t)(int32_t)rd32( voice + 0x4c ) * gvol) >> 16);
    int32_t  pL = (int32_t)rd32( voice + 0x30 ), pR = (int32_t)rd32( voice + 0x34 );
    int32_t  gL = (int32_t)rd32( voice + 0x38 ), gR = (int32_t)rd32( voice + 0x3c );
    int32_t  smooth = (int32_t)rd32( ND_MV_SMOOTH + (uint32_t)nd_slide );
    uint32_t roff   = rd32( ND_MV_RIGHTOFF + (uint32_t)nd_slide ) & 0xfffffffeu;
    uint32_t dest   = rd32( ND_MV_MIXDEST + (uint32_t)nd_slide );
    uint32_t i;

    for (i = 0; i < length; i++)
    {
        int isample0 = ((int)(int8_t)(rd8( sound + (pos >> 16) ) ^ 0x80)) << 8;
        pos += rate;
        {   int lvol = (int)(((int64_t)volume * pL) >> 16);
            int mix  = (int)(((int64_t)isample0 * lvol) >> 16) + (int16_t)rd16( dest );
            if (mix > 32767) mix = 32767; else if (mix < -32768) mix = -32768;
            wr16( dest, (uint16_t)(int16_t)mix ); }
        {   int rvol = (int)(((int64_t)volume * pR) >> 16);
            int mix  = (int)(((int64_t)isample0 * rvol) >> 16) + (int16_t)rd16( dest + roff );
            if (mix > 32767) mix = 32767; else if (mix < -32768) mix = -32768;
            wr16( dest + roff, (uint16_t)(int16_t)mix ); }
        dest += 4;
        pL = pL + (int)(((int64_t)(gL - pL) * smooth) >> 16);
        pR = pR + (int)(((int64_t)(gR - pR) * smooth) >> 16);
    }
    wr32( voice + 0x30, (uint32_t)pL );
    wr32( voice + 0x34, (uint32_t)pR );
    wr32( ND_MV_MIXDEST + (uint32_t)nd_slide, dest );

    gret( c, pos );                    /* return value: the advanced sample position */
    c->eip = rd32( esp );              /* cdecl: caller cleans the two args */
    c->regs[ESP] = esp + 4;
    return 1;
}

static void nat_arm_mixstereo( void )
{
    uint32_t b = ND_MIXSTEREO + (uint32_t)nd_slide;
    unsigned i;
    for (i = 0; i < sizeof(nd_mixstereo_code)/sizeof(nd_mixstereo_code[0]); i++)
        if (nd_mixstereo_code[i] >= 0 && rd8( b + i ) != (uint8_t)nd_mixstereo_code[i])
        { fprintf( stderr, "wasm_x86: MV_MixStereo skeleton differs at %08x - left interpreted\n", b ); return; }
    nat_register( b, NAT_MIXSTEREO, "MV_MixStereo<u8,s16>" );
}

/* mimalloc debug overhead.  The bundled mimalloc was built with MI_STAT and
 * MI_PADDING on, so every allocation runs mi_stat_update (atomic counter bumps
 * feeding MIMALLOC_SHOW_STATS) and every free runs mi_check_padding (decodes
 * the guard padding to detect overflow).  Both are void, purely diagnostic, and
 * take their args in registers (regparm, no stack args) - so replacing each with
 * an immediate return is functionally transparent (you lose only the stats
 * report and the corruption check, neither of which anything here reads) and
 * cannot unbalance the stack.  They fire on the allocation-heavy paths - level
 * load (texcache + palookups) most of all - so skipping them speeds loading and
 * trims a few % of steady render too.  Skeleton-verified; gated WASM_NO_MIDEBUG.
 * Any convention mistake would corrupt the very first free, so a rendering menu
 * is itself the proof it is right. */
#define ND_MI_STATUPD  0x5c9440u   /* mi_stat_update(stat<-eax, amount<-edx:ecx) */
#define ND_MI_CHKPAD   0x5c2720u   /* mi_check_padding(page<-eax, block<-edx)    */
static const int16_t nd_mistatupd_code[] = {
    0x55, 0x89,0xe5, 0x57, 0x56, 0x89,0xc6, 0x53, 0x83,0xec,0x24,   /* push ebp;mov esp,ebp;push edi,esi;mov eax,esi;push ebx;sub 0x24,esp */
    0x89,0x55,0xe0, 0x89,0x4d,0xe4                                  /* mov edx,-0x20(ebp); mov ecx,-0x1c(ebp)  (int64 amount) */
};
static const int16_t nd_michkpad_code[] = {
    0x55, 0x89,0xe5, 0x56, 0x53, 0x89,0xd3, 0x8d,0x55,0xf0,         /* push ebp;mov esp,ebp;push esi,ebx;mov edx,ebx;lea -0x10(ebp),edx */
    0x8d,0x4d,0xf4, 0x83,0xec,0x30, 0x89,0x14,0x24                  /* lea -0xc(ebp),ecx; sub 0x30,esp; mov edx,(esp) */
};

/* Immediate return for a regparm void function: pop the return address, leave
 * every register untouched (so any caller-saved/scratch expectation holds). */
static int nat_noop_ret( struct x86cpu *c )
{
    uint32_t esp = c->regs[ESP];
    c->eip = rd32( esp );
    c->regs[ESP] = esp + 4;
    return 1;
}

static void nat_arm_one( uint32_t va, const int16_t *code, unsigned n, const char *what )
{
    uint32_t b = va + (uint32_t)nd_slide;
    unsigned i;
    for (i = 0; i < n; i++)
        if (code[i] >= 0 && rd8( b + i ) != (uint8_t)code[i])
        { fprintf( stderr, "wasm_x86: %s skeleton differs at %08x - left interpreted\n", what, b ); return; }
    nat_register( b, NAT_MIDEBUG, what );
}

static void nat_arm_midebug( void )
{
    nat_arm_one( ND_MI_STATUPD, nd_mistatupd_code,
                 sizeof(nd_mistatupd_code)/sizeof(nd_mistatupd_code[0]), "mi_stat_update (noop)" );
    nat_arm_one( ND_MI_CHKPAD, nd_michkpad_code,
                 sizeof(nd_michkpad_code)/sizeof(nd_michkpad_code[0]), "mi_check_padding (noop)" );
}

/* Generic GL-thunk bypass.  The guest opengl32 entry is WINAPI (stdcall) and,
 * for every function we hook, takes N 4-byte args (int/float/enum/pointer - and
 * since guest_base is 0 a guest pointer is already a valid native pointer).  So
 * rebuild the native params struct { TEB *teb; arg0..argN } straight from the
 * guest stack and call the same native gl_* thunk the unix-call would have
 * reached - skipping only the interpreted marshalling.  stdcall: pop the return
 * address plus N*4 arg bytes; the guest wrappers are void so EAX is ignored. */
static int nat_glthunk( struct x86cpu *c )
{
    unsigned slot  = NAT_SLOT( c->eip );
    unsigned code  = g_gl_code[slot];
    unsigned nargs = g_gl_nargs[slot];
    uint32_t esp   = c->regs[ESP];
    uint32_t buf[20];
    unsigned i, j;
    if (g_glcount < 0) g_glcount = getenv( "WASM_GLCOUNT" ) ? 1 : 0;
    if (g_glcount) g_gl_calls[slot]++;
    buf[0] = c->fs_base;                                  /* TEB *teb */
    for (i = 0; i < nargs; i++) buf[1 + i] = rd32( esp + 4 + i * 4 );
    ((unsigned int (**)(void *))g_ogl_handle)[code]( buf );
    c->regs[EAX] = 0;
    c->eip = rd32( esp );
    c->regs[ESP] = esp + 4 + nargs * 4;
    return 1;
}

/* wglSwapBuffers is the one hot wrapper in this family with a return value.
 * Its unix params are { TEB *, HDC, BOOL ret }, so keep the output slot and
 * return it to the guest instead of using nat_glthunk's void convention. */
static int nat_glthunk_ret_code( struct x86cpu *c, unsigned code )
{
    uint32_t esp = c->regs[ESP], buf[3];
    buf[0] = c->fs_base;
    buf[1] = rd32( esp + 4 );
    buf[2] = 0;
    ((unsigned int (**)(void *))g_ogl_handle)[code]( buf );
    c->regs[EAX] = buf[2];
    c->eip = rd32( esp );
    c->regs[ESP] = esp + 8;
    return 1;
}
static int nat_glthunk_ret( struct x86cpu *c )
{ return nat_glthunk_ret_code( c, g_gl_code[NAT_SLOT( c->eip )] ); }
static int nat_wglswap( struct x86cpu *c )
{ return nat_glthunk_ret_code( c, 8 ); }

/* WIN_GL_SwapWindow is a small cdecl wrapper around the already-native
 * wglSwapBuffers thunk.  Its dynamic call is a persistent AOT boundary, so
 * avoid reinterpreting the wrapper when the exact target is the validated
 * wgl export.  A false host result resumes the wrapper after its call, which
 * preserves SDL's existing error path. */
#define ND_WIN_GL_SWAP 0x7a9fd0u
static int nat_win_gl_swapwindow( struct x86cpu *c )
{
    uint32_t sp = c->regs[ESP], window = rd32( sp + 8u );
    uint32_t driver, hdc, target, callsp;
    int result;

    if (!g_ogl_handle || !g_wgl_swap_addr || !window ||
        window >= NAT_GUEST_END - 0xa8u) return 0;
    driver = rd32( window + 0xa4u );
    if (!driver || driver >= NAT_GUEST_END - 0x10u) return 0;
    hdc = rd32( driver + 0x0cu );
    target = rd32( 0x1acfb64u + (uint32_t)nd_slide );
    if (!hdc || target != g_wgl_swap_addr) return 0;

    /* At entry the wrapper stack is [ret,arg0,arg1].  The target call needs
     * [ret-continuation, hdc], and stdcall returns with ESP back at `sp`. */
    callsp = sp - 8u;
    if (callsp < 0x20000u) return 0;
    wr32( callsp, ND_WIN_GL_SWAP + 0x19u + (uint32_t)nd_slide );
    wr32( callsp + 4u, hdc );
    c->regs[ESP] = callsp;
    c->eip = target;
    if (!nat_glthunk_ret_code( c, 8 )) { c->regs[ESP] = sp; c->eip = ND_WIN_GL_SWAP + (uint32_t)nd_slide; return 0; }
    result = c->regs[EAX] != 0;
    if (result)
    {
        c->regs[EAX] = 0;
        c->eip = rd32( sp );
        c->regs[ESP] = sp + 4u;
        return 1;
    }
    c->eip = ND_WIN_GL_SWAP + 0x19u + (uint32_t)nd_slide;
    c->regs[ESP] = sp;
    return 1;
}

static void nat_arm_win_gl_swapwindow( void )
{
    static const uint8_t code[] = {
        0x83,0xec,0x04, 0x8b,0x44,0x24,0x0c, 0x8b,0x80,0xa4,0x00,0x00,0x00,
        0x8b,0x40,0x0c, 0x89,0x04,0x24, 0xff,0x15,0,0,0,0,
        0x83,0xec,0x04, 0x85,0xc0,0x74,0x10, 0x31,0xc0,0x83,0xc4,0x04,0xc3
    };
    uint32_t b = ND_WIN_GL_SWAP + (uint32_t)nd_slide;
    unsigned i;
    for (i = 0; i < sizeof(code); i++)
        if (i < 21 || i > 24)
            if (rd8( b + i ) != code[i])
            { fprintf( stderr, "wasm_x86: WIN_GL_SwapWindow skeleton differs at %08x+%u - left interpreted\n", b, i ); return; }
    if (!getenv( "WASM_NO_WIN_GL_SWAP" ))
        nat_register( b, NAT_WIN_GL_SWAP, "WIN_GL_SwapWindow fast path" );
}

/* BoxedWine's pthread spin unlock is deliberately tiny and non-blocking:
 * store -1 to the guest word and return zero (cdecl).  Keep spin_lock itself
 * interpreted because its contention loop is synchronization-sensitive. */
#define ND_PTHREAD_SPIN_UNLOCK 0x809330u
static int nat_pthread_spin_unlock( struct x86cpu *c )
{
    uint32_t sp = c->regs[ESP], p = rd32( sp + 4u );
    if (!p || p >= NAT_GUEST_END - 4u) return 0;
    wr32( p, 0xffffffffu );
    return nat_ret_eax( c, sp, 0 );
}

static void nat_arm_pthread_spin_unlock( void )
{
    static const uint8_t code[] = {
        0x8b,0x44,0x24,0x04, 0xc7,0x00,0xff,0xff,0xff,0xff,
        0x31,0xc0,0xc3
    };
    uint32_t b = ND_PTHREAD_SPIN_UNLOCK + (uint32_t)nd_slide;
    unsigned i;
    for (i = 0; i < sizeof(code); i++)
        if (rd8( b + i ) != code[i])
        { fprintf( stderr, "wasm_x86: pthread_spin_unlock skeleton differs at %08x+%u - left interpreted\n", b, i ); return; }
    if (!getenv( "WASM_NO_PTHREAD_SPIN_UNLOCK" ))
        nat_register( b, NAT_PTHREAD_SPIN_UNLOCK, "pthread_spin_unlock" );
}

/* The matching spin-lock entry performs an unconditional xchg and then waits
 * only when the old word is zero.  A CAS gives the identical uncontended
 * transition without touching a held lock; races and contention fall back to
 * the guest's pause/retry loop. */
#define ND_PTHREAD_SPIN_LOCK 0x8092e0u
static int nat_pthread_spin_lock( struct x86cpu *c )
{
    uint32_t sp = c->regs[ESP], p = rd32( sp + 4u ), old, expected;
    if (!p || p >= NAT_GUEST_END - 4u) return 0;
    old = __atomic_load_n( (uint32_t *)(uintptr_t)p, __ATOMIC_SEQ_CST );
    if (!old) return 0;
    expected = old;
    if (!__atomic_compare_exchange_n( (uint32_t *)(uintptr_t)p, &expected, 0,
                                      0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST )) return 0;
    return nat_ret_eax( c, sp, 0 );
}

static void nat_arm_pthread_spin_lock( void )
{
    static const uint8_t code[] = {
        0x8b,0x44,0x24,0x04, 0x31,0xc9, 0x89,0xca, 0x87,0x10,
        0x85,0xd2, 0x74,0x0a, 0x31,0xc0,0xc3, 0x8d,0xb4,0x26,
        0x00,0x00,0x00,0x00, 0xf3,0x90, 0x8b,0x10, 0x85,0xd2,
        0x74,0xf8, 0xeb,0xe4
    };
    uint32_t b = ND_PTHREAD_SPIN_LOCK + (uint32_t)nd_slide;
    unsigned i;
    for (i = 0; i < sizeof(code); i++)
        if (rd8( b + i ) != code[i])
        { fprintf( stderr, "wasm_x86: pthread_spin_lock skeleton differs at %08x+%u - left interpreted\n", b, i ); return; }
    if (!getenv( "WASM_NO_PTHREAD_SPIN_LOCK" ))
        nat_register( b, NAT_PTHREAD_SPIN_LOCK, "pthread_spin_lock uncontended path" );
}

/* Initialized pthread_getspecific is a read of the current thread's key
 * array, bracketed by a spin lock and a cancellation no-op check.  Keep all
 * setup/cancellation/contended cases in the guest and shortcut only the fully
 * initialized, unlocked case. */
#define ND_PTHREAD_GETSPECIFIC 0x80a800u
#define ND_PTHREAD_ONCE_STATE  0x1acd0f4u
#define ND_PTHREAD_TLS_KEY     0x83373cu
#define ND_PTHREAD_CANCEL_FLAG 0x1acd0fcu
static int nat_pthread_getspecific( struct x86cpu *c )
{
    uint32_t sp = c->regs[ESP], teb = c->fs_base;
    uint32_t tlskey, obj = 0, key, count, flags, values, value = 0;
    uint32_t lock;

    if (!teb || teb >= NAT_GUEST_END - 0xf98u ||
        rd32( ND_PTHREAD_ONCE_STATE + (uint32_t)nd_slide ) != 1u) return 0;
    tlskey = rd32( ND_PTHREAD_TLS_KEY + (uint32_t)nd_slide );
    if (tlskey < 64u)
        obj = rd32( teb + 0xe10u + tlskey * 4u );
    else if (tlskey < 320u)
    {
        uint32_t slots = rd32( teb + 0xf94u );
        if (slots && slots < NAT_GUEST_END - 256u)
            obj = rd32( slots + (tlskey - 64u) * 4u );
    }
    if (!obj || obj >= NAT_GUEST_END - 0x3cu) return 0;
    lock = __atomic_load_n( (uint32_t *)(uintptr_t)(obj + 0x38u), __ATOMIC_SEQ_CST );
    if (lock != 0xffffffffu) return 0;

    key = rd32( sp + 4u );
    count = rd32( obj + 0x28u );
    if (key < count)
    {
        flags = rd32( obj + 0x30u );
        values = rd32( obj + 0x2cu );
        if (flags && values && key < NAT_GUEST_END - flags &&
            values < NAT_GUEST_END - 4u && key <= (NAT_GUEST_END - values) / 4u &&
            rd8( flags + key ) != 0)
            value = rd32( values + key * 4u );
    }
    if (__atomic_load_n( (uint32_t *)(uintptr_t)(obj + 0x38u), __ATOMIC_SEQ_CST ) != lock)
        return 0;

    /* pthread_getspecific calls pthread_testcancel after unlocking.  Its
     * entry returns immediately when cancellation is disabled or no request
     * is pending; require exactly that observable condition before eliding it. */
    if (!(rd8( obj + 0x20u ) & 0x0cu) &&
        rd32( ND_PTHREAD_CANCEL_FLAG + (uint32_t)nd_slide )) return 0;
    return nat_ret_eax( c, sp, value );
}

static void nat_arm_pthread_getspecific( void )
{
    static const uint8_t head[] = { 0x55,0x57,0x56,0x53,0x83,0xec,0x1c };
    uint32_t b = ND_PTHREAD_GETSPECIFIC + (uint32_t)nd_slide;
    unsigned i;
    for (i = 0; i < sizeof(head); i++)
        if (rd8( b + i ) != head[i])
        { fprintf( stderr, "wasm_x86: pthread_getspecific skeleton differs at %08x+%u - left interpreted\n", b, i ); return; }
    if (rd8( b + 0x0du ) != 0x89 || rd8( b + 0x0eu ) != 0xc6 || rd8( b + 0x14u ) != 0xa1)
    { fprintf( stderr, "wasm_x86: pthread_getspecific body differs at %08x - left interpreted\n", b ); return; }
    /* Keep this experimental shortcut opt-in.  It can alter the guest's
     * cancellation/ TLS ordering, so a normal browser run must stay on the
     * interpreter until it has been proven against the complete startup and
     * render path. */
    if (getenv( "WASM_PTHREAD_GETSPECIFIC" ) && !getenv( "WASM_NO_PTHREAD_GETSPECIFIC" ))
        nat_register( b, NAT_PTHREAD_GETSPECIFIC, "pthread_getspecific cached path" );
}

/* Core opengl32 entry points worth bypassing (exported, 4-byte args).  Sized by
 * the WASM_GLCOUNT tally: the fixed-function matrix stack and the per-draw state
 * dominate.  code = index into opengl32's __wine_unix_call_funcs. */
static const struct { const char *name; uint16_t code; uint8_t nargs; } nd_glthunks[] = {
    { "glMatrixMode",  183, 1 }, { "glPushMatrix", 219, 0 }, { "glPopMatrix",   214, 0 },
    { "glLoadMatrixf", 168, 1 }, { "glMultMatrixf",185, 1 }, { "glLoadIdentity",166, 0 },
    { "glEnable",       81, 1 }, { "glDisable",     72, 1 }, { "glBlendFunc",    16, 2 },
    { "glColor4f",      46, 4 }, { "glDrawArrays",  74, 3 }, { "glBindTexture",  14, 2 },
    { "glDepthFunc",    69, 1 }, { "glTexSubImage2D", 316, 9 },
};

static void nat_arm_glthunks( struct x86cpu *c )
{
    unsigned k;
    if (g_gl_armed) return;
    if (!g_ogl_base)
    {
        g_ogl_base = find_module( c, "opengl32.dll" );
        if (g_ogl_base)
        {
            uint32_t pe = g_ogl_base + rd32( g_ogl_base + 0x3c );
            g_ogl_size = rd32( pe + 0x50 );              /* SizeOfImage */
        }
    }
    if (!g_ogl_base || !g_ogl_handle) return;            /* wait for the handle capture */
    for (k = 0; k < sizeof(nd_glthunks)/sizeof(nd_glthunks[0]); k++)
    {
        uint32_t a = pe_export( g_ogl_base, nd_glthunks[k].name );
        if (!a) { fprintf( stderr, "wasm_x86: GL %s not exported - skipped\n", nd_glthunks[k].name ); continue; }
        nat_register( a, NAT_GLTHUNK, nd_glthunks[k].name );
        if (g_nat_addr[NAT_SLOT(a)] == a)                /* registered (not a slot clash) */
        { unsigned s = NAT_SLOT(a); g_gl_code[s] = nd_glthunks[k].code; g_gl_nargs[s] = nd_glthunks[k].nargs; }
    }
    {
        uint32_t a = pe_export( g_ogl_base, "wglSwapBuffers" );
        if (a)
        {   /* This address collides in the compact native table on the current
             * opengl32 image.  Keep it as a separate exact-address hook so
             * registering it cannot replace another native function. */
            g_wgl_swap_addr = a;
            fprintf( stderr, "wasm_x86: native wglSwapBuffers @ %08x (exact)\n", a );
        }
    }
    g_gl_armed = 1;
    nat_arm_win_gl_swapwindow();
    fprintf( stderr, "wasm_x86: GL-thunk bypass armed (opengl32=%08x handle=%08x)\n",
             g_ogl_base, (uint32_t)g_ogl_handle );
}

/* The hot GL 2.0+/3.x entry points are extension functions - not opengl32
 * exports, so pe_export can't see them; they are resolved through
 * wglGetProcAddress.  netduke32 uses GLAD, which parks each resolved thunk
 * pointer in a `glad_gl*` global (exe .bss, so nd_slide applies).  Read the
 * pointer straight from there and hook the thunk it names.  { glad global VA,
 * unix code, arg count }.  These were the top of the WASM_GLCOUNT tally
 * (glBindSampler, glUniform*, glVertexAttribPointer, ...). */
static const struct { uint32_t glad; uint16_t code; uint8_t nargs; const char *name; } nd_glext[] = {
    { 0x018e0c80, 350, 1, "glActiveTexture" }, { 0x019c6164, 417, 2, "glBindSampler" },
    { 0x018e0c7c,2645, 1, "glUseProgram" },    { 0x018e10d8,2924, 6, "glVertexAttribPointer" },
    { 0x018e10d4, 825, 1, "glEnableVertexAttribArray" },
    { 0x010d3a24,2520, 2, "glUniform1f" },     { 0x018dfb14,2542, 3, "glUniform2f" },
    { 0x019faae4,2627, 4, "glUniformMatrix4fv" }, { 0x018e0c84,2586, 5, "glUniform4f" },
    { 0x019c617c, 387, 2, "glBindBuffer" },    { 0x018e10dc, 479, 4, "glBufferData" },
    { 0x019fcd40, 487, 4, "glBufferSubData" },
};
static int g_glext_armed;

/* GLAD fills its glad_gl* pointers lazily during GL setup - NOT all at once - so
 * a single-shot arm races and misses whichever are still NULL that tick (and it
 * differs run to run).  Retry every tick, arming each entry the moment its
 * pointer resolves; a per-entry "done" bit means each is handled exactly once.
 * Stop once all are done or after a generous number of attempts (some entries
 * may name an extension the driver never provides, staying NULL forever). */
static uint32_t g_glext_done, g_glext_tries;

static void nat_arm_glext( struct x86cpu *c )
{
    unsigned k, n = sizeof(nd_glext)/sizeof(nd_glext[0]);
    (void)c;
    if (g_glext_armed || !g_ogl_handle) return;
    for (k = 0; k < n; k++)
    {
        uint32_t a;
        if (g_glext_done & (1u << k)) continue;
        a = rd32( nd_glext[k].glad + (uint32_t)nd_slide );
        if (!a || a >= 0x70000000u) continue;            /* not resolved yet - retry next tick */
        g_glext_done |= (1u << k);
        if (g_nat_addr[NAT_SLOT(a)] && g_nat_addr[NAT_SLOT(a)] != a)
        { fprintf( stderr, "wasm_x86: GL ext %s slot clash @%08x - not bypassed\n", nd_glext[k].name, a ); continue; }
        nat_register( a, NAT_GLTHUNK, nd_glext[k].name );
        if (g_nat_addr[NAT_SLOT(a)] == a)
        { unsigned s = NAT_SLOT(a); g_gl_code[s] = nd_glext[k].code; g_gl_nargs[s] = nd_glext[k].nargs; }
    }
    if (g_glext_done == (1u << n) - 1u || ++g_glext_tries > 200000)
    {
        g_glext_armed = 1;
        fprintf( stderr, "wasm_x86: GL-thunk ext bypass armed (done=%03x)\n", g_glext_done );
    }
}

static int nat_floor( struct x86cpu *c );
static int nat_pow( struct x86cpu *c );
static void nat_arm_pow( void );

#define ND_SETUP_VLINE  0x631ba0u
#define ND_SETUP_PVLINE 0x631c00u
#define ND_SETUP_MVLINE 0x631c60u
#define ND_SETUP_TVLINE 0x631c80u

/* The four Build mapper setup routines are tiny self-modifying writers.  The
 * frame profile's 0x631bfe representative is the return path of this family,
 * not the expensive mapper body.  Reproduce the writes directly, preserving
 * the cdecl return and the observable EAX/flags result of setupvlineasm. */
static int nat_setup_mapper( struct x86cpu *c, int kind )
{
    uint32_t sp = c->regs[ESP], a = rd32( sp + 4 ), b = (uint32_t)nd_slide;
    uint8_t al = (uint8_t)a;

    if (kind == NAT_SETUP_VLINE)
    {
        uint8_t neg = (uint8_t)-al, ah = (uint8_t)(al - 0x10);
        uint32_t mask = (1u << (neg & 31)) - 1u;
        wr8( 0x631cb5u+b, al ); wr8( 0x631cfbu+b, al );
        wr8( 0x6321f5u+b, al ); wr8( 0x63221cu+b, al );
        wr8( 0x632278u+b, al ); wr8( 0x63227du+b, al );
        wr8( 0x63228bu+b, ah ); wr8( 0x632286u+b, neg );
        wr8( 0x632150u+b, neg ); wr8( 0x632153u+b, neg );
        wr8( 0x6321bbu+b, neg ); wr8( 0x6321beu+b, neg );
        wr32( 0x6321fau+b, mask ); wr32( 0x632230u+b, mask );
        c->regs[EAX] = mask;
        set_lazy( c, K_DEC, 1u << (neg & 31), 1, mask, 4 );
    }
    else if (kind == NAT_SETUP_MVLINE)
    {
        wr8( 0x631db4u+b, al ); wr8( 0x632604u+b, al );
        wr8( 0x6325e5u+b, al ); wr8( 0x6325b1u+b, al );
        wr8( 0x6325aeu+b, al ); c->regs[EAX] = a;
    }
    else if (kind == NAT_SETUP_TVLINE)
    {
        wr8( 0x632004u+b, al ); c->regs[EAX] = a;
    }
    else if (kind == NAT_SETUP_PVLINE) /* prosetupvlineasm */
    {
        uint8_t neg = (uint8_t)-al, ah = (uint8_t)(al - 0x10);
        uint32_t mask = (1u << (neg & 31)) - 1u;
        wr8( 0x631cb5u+b, al ); wr8( 0x631cfbu+b, al );
        wr8( 0x632405u+b, al ); wr8( 0x632438u+b, al );
        wr8( 0x63249bu+b, al ); wr8( 0x6324a0u+b, al );
        wr8( 0x6324aeu+b, ah ); wr8( 0x6324a9u+b, neg );
        wr8( 0x632365u+b, neg ); wr8( 0x632368u+b, neg );
        wr8( 0x6323cdu+b, neg ); wr8( 0x6323d0u+b, neg );
        wr32( 0x63240au+b, mask ); wr32( 0x632449u+b, mask );
        c->regs[EAX] = mask;
        set_lazy( c, K_DEC, 1u << (neg & 31), 1, mask, 4 );
    }
    else return 0;
    c->regs[ESP] = sp + 4; c->eip = rd32( sp );
    return 1;
}

static void nat_arm_setup_mappers( void )
{
    static const struct { uint32_t a, ret; int kind; const char *name; } t[] = {
        { ND_SETUP_VLINE,  0x5e, NAT_SETUP_VLINE,     "setupvlineasm" },
        { ND_SETUP_PVLINE, 0x5e, NAT_SETUP_PVLINE,  "prosetupvlineasm" },
        { ND_SETUP_MVLINE, 0x1d, NAT_SETUP_MVLINE,     "setupmvlineasm" },
        { ND_SETUP_TVLINE, 0x0d, NAT_SETUP_TVLINE,     "setuptvlineasm" },
    };
    if (!getenv( "WASM_SETUP_MAPPERS" )) return;
    for (unsigned i = 0; i < sizeof(t)/sizeof(t[0]); i++)
    {
        uint32_t a = t[i].a + (uint32_t)nd_slide;
        if (rd8(a) != 0x8b || rd8(a+1) != 0x44 || rd8(a+2) != 0x24 ||
            rd8(a+3) != 0x04 || rd8(a+4) != 0xa2 ||
            rd8(a+t[i].ret) != 0xc3)
            continue;
        nat_register( a, t[i].kind, t[i].name );
    }
}

static int nat_call( struct x86cpu *c, int kind )
{
    if (kind == NAT_CRC32)   return nat_crc32( c );
    if (kind == NAT_MIXSTEREO) return nat_mixstereo( c );
    if (kind == NAT_MIDEBUG)  return nat_noop_ret( c );
    if (kind == NAT_GLTHUNK)  return nat_glthunk( c );
    if (kind == NAT_GLTHUNK_RET) return nat_glthunk_ret( c );
    if (kind == NAT_PALMATCH) return nat_pal_closest( c );
    if (kind == NAT_AGELOOP) return nat_ageloop( c );
    if (kind == NAT_AGEBLOCKS) return nat_ageblocks( c );
    if (kind == NAT_SETUPQRHLINE) return nat_setupqrhline( c );
    if (kind == NAT_QRHLINE) return nat_qrhline( c );
    if (kind == NAT_VLINE_DISPATCH) return nat_vline_dispatch( c );
    if (kind == NAT_MVLINE_DISPATCH) return nat_mvline_dispatch( c );
    if (kind == NAT_VLINE)   return nat_vlineasm4( c );
    if (kind == NAT_MVLINE)  return nat_mvlineasm4( c );
    if (kind == NAT_MHLINE)  return nat_mhlineskip( c );
    if (kind == NAT_VLINE1)  return nat_vlineasm1( c );
    if (kind == NAT_MVLINE1) return nat_mvlineasm1( c );
    if (kind == NAT_VLINE1NP2) return nat_vlineasm1np2( c );
    if (kind == NAT_MVLINE1NP2) return nat_mvlineasm1np2( c );
    if (kind == NAT_GLSTATE) return nat_inthash_find( c );
    if (kind == NAT_GLSAMPLER) return nat_glsampler( c );
    if (kind == NAT_LIBDIV)   return nat_libdivide( c );
    if (kind == NAT_SURFSPAN) return nat_surfspan( c );
    if (kind == NAT_SURFBLIT) return nat_surfblit( c );
    if (kind == NAT_MEMCMP) return nat_memcmp( c );
    if (kind == NAT_STRCMP) return nat_strcmp( c );
    if (kind == NAT_STRLEN) return nat_strlen( c );
    if (kind == NAT_MEMCHR) return nat_memchr( c );
    if (kind == NAT_STRncmp) return nat_strncmp( c );
    if (kind == NAT_STRCHR) return nat_strchr( c );
    if (kind == NAT_STRCPY) return nat_strcpy( c );
    if (kind == NAT_STRNCPY) return nat_strncpy( c );
    if (kind == NAT_WCSLEN) return nat_wcslen( c );
    if (kind == NAT_WCSCHR) return nat_wcschr( c );
    if (kind == NAT_MEMSET_LOOP) return nat_memset_loop( c );
    if (kind == NAT_QPF) return nat_qpf( c );
    if (kind == NAT_QPC) return nat_qpc( c );
    if (kind == NAT_GETTID) return nat_gettid( c );
    if (kind == NAT_TLSGETVALUE) return nat_tlsgetvalue( c );
    if (kind == NAT_SDL_TLSGET) return nat_sdl_tlsget( c );
    if (kind == NAT_TOASCII) return nat_toascii( c );
    if (kind == NAT_UDIVMODDI4) return nat_udivmoddi4( c );
    if (kind == NAT_TOLOWER) return nat_casefold( c, 0 );
    if (kind == NAT_TOUPPER) return nat_casefold( c, 1 );
    if (kind == NAT_SDL_ATOMIC_GET || kind == NAT_SDL_ATOMIC_GETPTR)
        return nat_sdl_atomic_get( c );
    if (kind == NAT_SDL_ATOMIC_XADD) return nat_sdl_atomic_xadd( c );
    if (kind == NAT_NTDLL_SRW_EXCL) return nat_ntdll_srw_excl( c );
    if (kind == NAT_NTDLL_SRW_SHARED) return nat_ntdll_srw_shared( c );
    if (kind == NAT_NTDLL_SRW_EXCL_REL) return nat_ntdll_srw_excl_rel( c );
    if (kind == NAT_NTDLL_WAKE_ALL_EMPTY) return nat_ntdll_wake_all_empty( c );
    if (kind == NAT_FLOOR) return nat_floor( c );
    if (kind == NAT_POW) return nat_pow( c );
    if (kind == NAT_WIN_GL_SWAP) return nat_win_gl_swapwindow( c );
    if (kind == NAT_PTHREAD_SPIN_UNLOCK) return nat_pthread_spin_unlock( c );
    if (kind == NAT_PTHREAD_SPIN_LOCK) return nat_pthread_spin_lock( c );
    if (kind == NAT_PTHREAD_GETSPECIFIC) return nat_pthread_getspecific( c );
    if (kind == NAT_GDI_RELAY) return nat_gdi_relay( c );
    if (kind == NAT_GDI_CLIENT) return nat_gdi_client_ptr( c );
    if (kind >= NAT_SETUP_VLINE && kind <= NAT_SETUP_TVLINE)
        return nat_setup_mapper( c, kind );
    switch (kind)
    {
    case NAT_SDL_OPEN:     return sdl_open_audio( c, 0 );
    case NAT_SDL_OPENDEV:  return sdl_open_audio( c, 1 );
    case NAT_SDL_PAUSE:    g_aud.paused = (int)garg( c, 0 ) != 0; gret( c, 0 ); return 1;
    case NAT_SDL_PAUSEDEV: g_aud.paused = (int)garg( c, 1 ) != 0; gret( c, 0 ); return 1;
    case NAT_SDL_LOCK:     g_aud.lock++; gret( c, 0 ); return 1;
    case NAT_SDL_UNLOCK:   if (g_aud.lock) g_aud.lock--; gret( c, 0 ); return 1;
    case NAT_SDL_CLOSE:    g_aud.open = 0; g_aud.paused = 1; gret( c, 0 ); return 1;
    case NAT_SDL_POLL:     return sdl_poll_event( c );
    case NAT_SDL_KEYBOARDSTATE: return sdl_keyboard_state( c );
    /* Say yes to relative mode: the game then uses xrel/yrel, which we supply.
     * Record the requested state - in relative (aiming) mode SDL never gets the
     * WM_MOUSEMOVE that would give its window mouse focus, so it drops the posted
     * WM_*BUTTON messages; sdl_poll_event synthesises the button events instead. */
    case NAT_SDL_RELMOUSE: g_rel_mouse_on = (int)garg( c, 0 ); gret( c, 0 ); return 1;
    default: break;
    }
    uint32_t esp = c->regs[ESP];
    uint32_t dst = rd32( esp + 4 ), a1 = rd32( esp + 8 ), n = rd32( esp + 12 );
    const uint32_t GUEST_END = 0x70000000u;              /* native wine lives above this */
    if (n > GUEST_END || dst >= GUEST_END || dst + n > GUEST_END) return 0;
    if (kind == NAT_MEMMOVE)
    {
        if (a1 >= GUEST_END || a1 + n > GUEST_END) return 0;
        memmove( (void *)(uintptr_t)dst, (const void *)(uintptr_t)a1, n );
    }
    else memset( (void *)(uintptr_t)dst, (int)(a1 & 0xff), n );
    c->regs[EAX] = dst;
    c->regs[ESP] = esp + 4;                              /* pop the return address */
    c->eip = rd32( esp );
    return 1;
}

static void prof_dump( void )
{
    /* dump every occupied slot, not a top-N: once the obvious hot loops are
     * gone the profile is flat, and a top-N list of a flat profile invites the
     * exact error of computing percentages against the listed subset */
    for (int top = 0; top < PROF_SLOTS; top++)
    {
        uint32_t best = 0, bi = 0;
        for (int i = 0; i < PROF_SLOTS; i++)
            if (g_prof_cnt[i] > best) { best = g_prof_cnt[i]; bi = i; }
        if (!best) break;
        if (g_prof_eip[bi] >= 0x401000 && g_prof_eip[bi] < 0x900000)
            fprintf( stderr, "PROF %08x %u\n", g_prof_eip[bi], best );
        else
            fprintf( stderr, "PROF %08x %u %s\n", g_prof_eip[bi], best, prof_module( g_prof_eip[bi] ) );
        g_prof_cnt[bi] = 0;   /* consumed */
    }
    fprintf( stderr, "PROF TOTAL %llu dropped %llu\n",
             (unsigned long long)g_prof_total, (unsigned long long)g_prof_dropped );
    fprintf( stderr, "PROF ---\n" );
    memset( g_prof_cnt, 0, sizeof(g_prof_cnt) );
    g_prof_total = 0; g_prof_dropped = 0;
}

static void wasm_dump_frame( struct x86cpu *c )
{
    (void)c;
#ifndef WEBWINE_BROWSER
    const char *path = getenv( "WASM_DUMP_FRAME" );
    if (!path || !*path) return;
#endif
    /* Prefer the live frameplace; at a page flip it is 0 (cleared by
     * videoEndDrawing), so fall back to the cached last-non-zero buffer, which
     * still holds the just-finished frame. */
    uint32_t fp  = rd32( ND_FRAMEPLACE + nd_slide );
    int w, h, bpl;
    if (fp) { w = (int)rd32( ND_XDIM + nd_slide ); h = (int)rd32( ND_YDIM + nd_slide );
              bpl = (int)rd32( ND_BYTESPERLINE + nd_slide ); }
    else    { fp = g_last_fp; w = g_last_w; h = g_last_h; bpl = g_last_bpl; }
    uint32_t pal = ND_CURPALETTE + nd_slide;
    if (!fp || w < 1 || h < 1 || w > 8192 || h > 8192 || bpl < w) return;
    size_t npix = (size_t)w * h;

    /* De-palettise into 32-bit pixels.  Two things matter here because this runs
     * inside the interpreter loop, once per rendered frame, so its cost comes
     * straight off the frame rate:
     *   - build a 256-entry palette LUT once per frame, so the per-pixel work is
     *     one indexed load + one store instead of four guest loads + three stores;
     *   - keep one grow-only buffer instead of malloc/free-ing several megabytes
     *     every single frame.
     * The pixels are RGBA (not RGB) so the browser can hand the bytes straight to
     * ImageData without a per-pixel conversion pass in JavaScript. */
    static uint32_t lut[256];
    for (int i = 0; i < 256; i++)
    {
        uint32_t pe = pal + (uint32_t)i * 4;
        lut[i] = (uint32_t)rd8( pe ) | ((uint32_t)rd8( pe+1 ) << 8) |
                 ((uint32_t)rd8( pe+2 ) << 16) | 0xff000000u;
    }

    /* WASM_FRAMESTAT=1: once a second, say what the frame is actually made of -
     * the commonest palette index and how many palette entries are pure white.
     * A blank screen is either the pixels or the palette and the picture alone
     * cannot tell you which. */
    { static int on = -1; static unsigned n;
      if (on == -1) on = getenv( "WASM_FRAMESTAT" ) ? 1 : 0;
      if (on && (n++ % 300) == 0)
      {
          unsigned hist[256] = {0}, white = 0, top = 0;
          for (int y = 0; y < h; y += 4)
              for (int x = 0; x < w; x += 4) hist[ rd8( fp + (uint32_t)y * (uint32_t)bpl + x ) ]++;
          for (int i = 0; i < 256; i++)
          {
              if (hist[i] > hist[top]) top = i;
              if ((lut[i] & 0xffffff) == 0xffffff) white++;
          }
          fprintf( stderr, "wasm_x86: framestat %dx%d top index %u (%u%%) pal[%u]=%06x pal[0]=%06x white entries %u\n",
                   w, h, top, hist[top] * 100 / (unsigned)(((h+3)/4) * ((w+3)/4)),
                   top, lut[top] & 0xffffff, lut[0] & 0xffffff, white );
      } }
    static uint32_t *fb;
    static size_t fb_px;
    if (npix > fb_px)
    {
        free( fb );
        fb = malloc( npix * 4 );
        fb_px = fb ? npix : 0;
        if (!fb) { fprintf( stderr, "wasm_x86: frame buffer alloc failed\n" ); return; }
    }
    for (int y = 0; y < h; y++)
    {
        uint32_t row = fp + (uint32_t)y * (uint32_t)bpl;
        uint32_t *dst = fb + (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++) dst[x] = lut[ rd8( row + x ) ];
    }
#ifdef WEBWINE_BROWSER
    /* Live display: hand the frame to the page (posts to the main thread). */
    extern void webwine_present( const void *rgba, int w, int h );
    webwine_present( fb, w, h );
    g_present_count++;
    return;
#else
    /* host fopen() is broken in this build (wine overrides open -> EDOM), so
     * emit the frame as base64 over stderr, which works.  Host side extracts
     * between the markers and rebuilds a PPM (P6 = 3 bytes/pixel). */
    size_t nrgb = npix * 3;
    unsigned char *rgb = malloc( nrgb );
    if (!rgb) { fprintf( stderr, "wasm_x86: frame malloc failed\n" ); return; }
    for (size_t i = 0; i < npix; i++)
    {
        uint32_t v = fb[i];
        rgb[i*3+0] = (unsigned char)(v);
        rgb[i*3+1] = (unsigned char)(v >> 8);
        rgb[i*3+2] = (unsigned char)(v >> 16);
    }
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t enclen = (nrgb + 2) / 3 * 4;
    char *enc = malloc( enclen + 1 );
    if (!enc) { free( rgb ); fprintf( stderr, "wasm_x86: b64 malloc failed\n" ); return; }
    size_t e = 0;
    for (size_t i = 0; i < nrgb; i += 3)
    {
        unsigned v = rgb[i] << 16;
        int n = 1;
        if (i + 1 < nrgb) { v |= rgb[i+1] << 8; n = 2; }
        if (i + 2 < nrgb) { v |= rgb[i+2];      n = 3; }
        enc[e++] = b64[(v >> 18) & 63];
        enc[e++] = b64[(v >> 12) & 63];
        enc[e++] = n > 1 ? b64[(v >> 6) & 63] : '=';
        enc[e++] = n > 2 ? b64[v & 63]        : '=';
    }
    enc[e] = 0;
    fprintf( stderr, "WASM_FRAME_BEGIN %d %d\n", w, h );
    fwrite( enc, 1, e, stderr );
    fprintf( stderr, "\nWASM_FRAME_END\n" );
    fflush( stderr );
    free( enc ); free( rgb );
    fprintf( stderr, "wasm_x86: emitted frame %dx%d (%zu b64 bytes)\n", w, h, e );
#endif  /* !WEBWINE_BROWSER */
}

/* ---- flags ---- */
static uint32_t parity( uint8_t v ){ v ^= v>>4; v ^= v>>2; v ^= v>>1; return (~v)&1; }

static uint32_t signmask( int size ){ return size==1?0x80:size==2?0x8000:0x80000000u; }
static uint32_t sizemask( int size ){ return size==1?0xff:size==2?0xffff:0xffffffffu; }

static void set_lazy( struct x86cpu *c, int kind, uint32_t op1, uint32_t op2, uint32_t res, int size )
{
    c->lf_kind = kind; c->lf_op1 = op1; c->lf_op2 = op2; c->lf_res = res & sizemask(size); c->lf_size = size;
}

static uint32_t get_flags( struct x86cpu *c )
{
    uint32_t f = c->eflags & ~(CF|PF|AF|ZF|SF|OF);
    uint32_t res = c->lf_res, op1 = c->lf_op1, op2 = c->lf_op2, sm = signmask(c->lf_size);
    if (c->lf_size == 0) return c->eflags;
    if (!res) f |= ZF;
    if (res & sm) f |= SF;
    if (parity((uint8_t)res)) f |= PF;
    switch (c->lf_kind)
    {
    case K_ADD:
        if (res < op1) f |= CF;
        if (((op1 ^ res) & (op2 ^ res)) & sm) f |= OF;
        if (((op1 ^ op2 ^ res) & 0x10)) f |= AF;
        break;
    case K_SUB:
        if (op1 < op2) f |= CF;
        if (((op1 ^ op2) & (op1 ^ res)) & sm) f |= OF;
        if (((op1 ^ op2 ^ res) & 0x10)) f |= AF;
        break;
    case K_ADC: /* res = op1 + op2 + cin */
        if (res < op1 || (res == op1 && c->lf_cin)) f |= CF;
        if (((op1 ^ res) & (op2 ^ res)) & sm) f |= OF;
        if (((op1 ^ op2 ^ res) & 0x10)) f |= AF;
        break;
    case K_SBB: /* res = op1 - op2 - cin */
        if (op1 < op2 || (op1 == op2 && c->lf_cin)) f |= CF;
        if (((op1 ^ op2) & (op1 ^ res)) & sm) f |= OF;
        if (((op1 ^ op2 ^ res) & 0x10)) f |= AF;
        break;
    case K_LOGIC: break; /* CF=OF=0 */
    case K_INCDEC: /* preserve CF from eflags */
        f |= (c->eflags & CF);
        if (((op1 ^ op2) & (op1 ^ res)) & sm) f |= OF;
        break;
    case K_INC: /* res = op1 + 1; CF preserved */
        f |= (c->eflags & CF);
        if (((op1 ^ res) & (op2 ^ res)) & sm) f |= OF;
        if (((op1 ^ op2 ^ res) & 0x10)) f |= AF;
        break;
    case K_DEC: /* res = op1 - 1; CF preserved */
        f |= (c->eflags & CF);
        if (((op1 ^ op2) & (op1 ^ res)) & sm) f |= OF;
        if (((op1 ^ op2 ^ res) & 0x10)) f |= AF;
        break;
    }
    return f;
}

/* Single-flag accessors.  get_flags() materializes ALL six arithmetic flags
 * (including the parity fold and the kind switch), but a condition code needs
 * only one or two of them — and cmp+jcc is the most common pair in the guest
 * (39/3b/81/83 feeding 74/75/0f84/0f8d, hundreds of millions per run).  Each
 * of these mirrors the corresponding branch of get_flags() exactly; they all
 * return 0/1 so the composite conditions below can compare them directly. */
static int lf_zf( struct x86cpu *c )
{
    if (!c->lf_size) return !!(c->eflags & ZF);
    return c->lf_res == 0;
}
static int lf_sf( struct x86cpu *c )
{
    if (!c->lf_size) return !!(c->eflags & SF);
    return !!(c->lf_res & signmask( c->lf_size ));
}
static int lf_pf( struct x86cpu *c )
{
    if (!c->lf_size) return !!(c->eflags & PF);
    return (int)parity( (uint8_t)c->lf_res );
}
static int lf_cf( struct x86cpu *c )
{
    if (!c->lf_size) return !!(c->eflags & CF);
    uint32_t res = c->lf_res, op1 = c->lf_op1, op2 = c->lf_op2;
    switch (c->lf_kind)
    {
    case K_ADD: return res < op1;
    case K_SUB: return op1 < op2;
    case K_ADC: return res < op1 || (res == op1 && c->lf_cin);
    case K_SBB: return op1 < op2 || (op1 == op2 && c->lf_cin);
    /* inc/dec preserve CF; logic clears it; K_NONE contributes nothing */
    case K_INCDEC: case K_INC: case K_DEC: return !!(c->eflags & CF);
    default: return 0;
    }
}
static int lf_of( struct x86cpu *c )
{
    if (!c->lf_size) return !!(c->eflags & OF);
    uint32_t res = c->lf_res, op1 = c->lf_op1, op2 = c->lf_op2, sm = signmask( c->lf_size );
    switch (c->lf_kind)
    {
    case K_ADD: case K_ADC: case K_INC:
        return !!(((op1 ^ res) & (op2 ^ res)) & sm);
    case K_SUB: case K_SBB: case K_INCDEC: case K_DEC:
        return !!(((op1 ^ op2) & (op1 ^ res)) & sm);
    default: return 0;
    }
}

static inline __attribute__((always_inline)) int cond( struct x86cpu *c, int cc )
{
    int res;
    switch (cc >> 1)
    {
    case 0: res = lf_of(c); break;                        /* O */
    case 1: res = lf_cf(c); break;                        /* B/C */
    case 2: res = lf_zf(c); break;                        /* Z/E */
    case 3: res = lf_cf(c) || lf_zf(c); break;            /* BE */
    case 4: res = lf_sf(c); break;                        /* S */
    case 5: res = lf_pf(c); break;                        /* P */
    case 6: res = lf_sf(c) != lf_of(c); break;            /* L */
    case 7: res = (lf_sf(c) != lf_of(c)) || lf_zf(c); break; /* LE */
    default: res = 0;
    }
    return (cc & 1) ? !res : res;
}

/* ---- decode helpers ---- */
struct decode
{
    int opsize;   /* 2 or 4 */
    int addrsize; /* 4 */
    int rep;      /* 0, 0xf2, 0xf3 */
    int seg;      /* segment override base reg, or -1 */
    uint32_t eip; /* running fetch pointer */
};

static uint8_t  f8 ( struct decode *d ){ return rd8 ( d->eip++ ); }
static uint16_t f16( struct decode *d ){ uint16_t v = rd16( d->eip ); d->eip += 2; return v; }
static uint32_t f32( struct decode *d ){ uint32_t v = rd32( d->eip ); d->eip += 4; return v; }

/* ModRM: returns effective address (for mem) or, for reg, sets *is_reg + reg num.
 * Also returns the reg field (opcode /r). */
struct modrm { int mod, reg, rm; int is_reg; uint32_t ea; };

static struct modrm decode_modrm( struct x86cpu *c, struct decode *d )
{
    struct modrm m;
    uint8_t b = f8( d );
    m.mod = b >> 6; m.reg = (b >> 3) & 7; m.rm = b & 7; m.is_reg = 0; m.ea = 0;
    if (m.mod == 3) { m.is_reg = 1; return m; }

    uint32_t base = 0; int have_base = 0;
    if (m.rm == 4) /* SIB */
    {
        uint8_t sib = f8( d );
        int scale = sib >> 6, idx = (sib >> 3) & 7, bas = sib & 7;
        uint32_t index = (idx == 4) ? 0 : (c->regs[idx] << scale);
        if (bas == 5 && m.mod == 0) { base = f32( d ); }
        else { base = c->regs[bas]; have_base = 1; }
        m.ea = base + index;
        (void)have_base;
    }
    else if (m.rm == 5 && m.mod == 0) { m.ea = f32( d ); }
    else { m.ea = c->regs[m.rm]; }

    if (m.mod == 1) m.ea += (int8_t)f8( d );
    else if (m.mod == 2) m.ea += (int32_t)f32( d );
    m.ea += (uint32_t)d->seg;   /* segment base (fs->TEB, gs, else 0) */
    return m;
}

static uint32_t read_rm( struct x86cpu *c, struct modrm *m, int size )
{
    if (m->is_reg)
    {
        uint32_t r = c->regs[size==1 ? (m->rm & 3) : m->rm];
        if (size==1) return (m->rm < 4) ? (r & 0xff) : ((c->regs[m->rm & 3] >> 8) & 0xff);
        return r & sizemask(size);
    }
    return size==1 ? rd8(m->ea) : size==2 ? rd16(m->ea) : rd32(m->ea);
}

static void write_rm( struct x86cpu *c, struct modrm *m, int size, uint32_t v )
{
    if (m->is_reg)
    {
        if (size==1)
        {
            if (m->rm < 4) c->regs[m->rm] = (c->regs[m->rm] & 0xffffff00) | (v & 0xff);
            else c->regs[m->rm & 3] = (c->regs[m->rm & 3] & 0xffff00ff) | ((v & 0xff) << 8);
        }
        else if (size==2) c->regs[m->rm] = (c->regs[m->rm] & 0xffff0000) | (v & 0xffff);
        else c->regs[m->rm] = v;
        return;
    }
    if (size==1) wr8(m->ea, v); else if (size==2) wr16(m->ea, v); else wr32(m->ea, v);
}

static uint32_t read_reg( struct x86cpu *c, int reg, int size )
{
    if (size==1) return reg < 4 ? (c->regs[reg] & 0xff) : ((c->regs[reg & 3] >> 8) & 0xff);
    return c->regs[reg] & sizemask(size);
}
static void write_reg( struct x86cpu *c, int reg, int size, uint32_t v )
{
    if (size==1) { if (reg < 4) c->regs[reg] = (c->regs[reg] & 0xffffff00)|(v&0xff);
                   else c->regs[reg&3] = (c->regs[reg&3] & 0xffff00ff)|((v&0xff)<<8); }
    else if (size==2) c->regs[reg] = (c->regs[reg] & 0xffff0000)|(v&0xffff);
    else c->regs[reg] = v;
}

static void push32( struct x86cpu *c, uint32_t v ){ c->regs[ESP] -= 4; wr32( c->regs[ESP], v ); }
static uint32_t pop32( struct x86cpu *c ){ uint32_t v = rd32( c->regs[ESP] ); c->regs[ESP] += 4; return v; }

/* ---- x87 FPU ---- */
static inline double fp_get( struct x86cpu *c, int i ) { return c->fpr[(c->fptop + i) & 7]; }
static inline void   fp_set( struct x86cpu *c, int i, double v ) { c->fpr[(c->fptop + i) & 7] = v; }
static inline void   fp_push( struct x86cpu *c, double v ) { c->fptop = (c->fptop - 1) & 7; c->fpr[c->fptop] = v; }
static inline double fp_pop( struct x86cpu *c ) { double v = c->fpr[c->fptop]; c->fptop = (c->fptop + 1) & 7; return v; }

/* 80-bit extended <-> double (long double is 64-bit on arm64, so do it by hand) */
static double rd80( uint32_t a )
{
    uint64_t m = rd32(a) | ((uint64_t)rd32(a+4) << 32);
    uint16_t se = rd16(a+8);
    int sign = se >> 15, exp = se & 0x7fff;
    double v;
    if (exp == 0 && m == 0) return sign ? -0.0 : 0.0;
    if (exp == 0x7fff) return m == 0 ? (sign ? -INFINITY : INFINITY) : NAN;
    v = ldexp( (double)m, exp - 16383 - 63 );
    return sign ? -v : v;
}
static void wr80( uint32_t a, double v )
{
    uint64_t m; uint16_t se; int e;
    if (v == 0.0) { m = 0; se = signbit(v) ? 0x8000 : 0; }
    else if (isinf(v)) { m = 0; se = 0x7fff | (v < 0 ? 0x8000 : 0); }
    else if (isnan(v)) { m = 0xc000000000000000ULL; se = 0x7fff; }
    else {
        int sign = signbit(v); double f = frexp( fabs(v), &e );  /* f in [0.5,1) */
        m = (uint64_t)ldexp( f, 64 );
        se = (uint16_t)((e + 16382) & 0x7fff) | (sign ? 0x8000 : 0);
    }
    wr32( a, (uint32_t)m ); wr32( a+4, (uint32_t)(m >> 32) ); wr16( a+8, se );
}
static double rdf32( uint32_t a ) { uint32_t u = rd32(a); float f; memcpy(&f,&u,4); return (double)f; }
static double rdf64( uint32_t a ) { uint64_t u = rd32(a) | ((uint64_t)rd32(a+4)<<32); double d; memcpy(&d,&u,8); return d; }
/* ntdll's floor is a cdecl double -> ST(0) helper.  Keep this opt-in until
 * the guest/host exceptional-value matrix is checked; the stack transition is
 * explicit so it cannot be mistaken for an integer-returning CRT hook. */
static int nat_floor( struct x86cpu *c )
{
    uint32_t esp = c->regs[ESP];
    if (esp > NAT_GUEST_END - 12) return 0;
    fp_push( c, floor( rdf64( esp + 4 ) ) );
    c->regs[ESP] = esp + 4;
    c->eip = rd32( esp );
    return 1;
}
/* The executable's _pow is a large libmingw32 x87 routine.  The emulator's
 * x87 representation is double precision, so host pow() has the same visible
 * precision; push the result as ST(0), exactly like the guest cdecl return. */
static int nat_pow( struct x86cpu *c )
{
    uint32_t esp = c->regs[ESP];
    if (esp > NAT_GUEST_END - 20) return 0;
    fp_push( c, pow( rdf64( esp + 4 ), rdf64( esp + 12 ) ) );
    c->regs[ESP] = esp + 4;         /* caller removes the two double args */
    c->eip = rd32( esp );
    return 1;
}

static void nat_arm_pow( void )
{
    /* _pow at 0x7b8360: push esi; push ebx; sub esp,0x44; fld qword args. */
    static const uint8_t head[] = {
        0x56,0x53,0x83,0xec,0x44,0xdd,0x44,0x24,0x50,
        0xdd,0x44,0x24,0x58
    };
    uint32_t a = 0x7b8360u + (uint32_t)nd_slide;
    unsigned i;
    if (getenv( "WASM_NO_POW" )) return;
    for (i = 0; i < sizeof(head); i++) if (rd8( a + i ) != head[i]) break;
    if (i == sizeof(head)) nat_register( a, NAT_POW, "pow (x87 return)" );
    else fprintf( stderr, "wasm_x86: pow skeleton differs at %08x+%x - left interpreted\n", a, i );
}
static void   wrf32( uint32_t a, double v ) { float f = (float)v; uint32_t u; memcpy(&u,&f,4); wr32(a,u); }
static void   wrf64( uint32_t a, double v ) { uint64_t u; memcpy(&u,&v,8); wr32(a,(uint32_t)u); wr32(a+4,(uint32_t)(u>>32)); }
/* set condition codes C3,C2,C0 in the status word from a compare of a vs b */
static void fp_compare( struct x86cpu *c, double a, double b )
{
    c->fpsw &= ~0x4500;  /* clear C3(0x4000) C2(0x400) C0(0x100) */
    if (isnan(a) || isnan(b)) c->fpsw |= 0x4500;         /* unordered */
    else if (a < b)           c->fpsw |= 0x0100;         /* C0 */
    else if (a == b)          c->fpsw |= 0x4000;         /* C3 */
    /* a > b: all clear */
}
/* set EFLAGS ZF/PF/CF from an ordered compare (fcomi/fucomi) */
static void fp_compare_eflags( struct x86cpu *c, double a, double b )
{
    c->eflags &= ~(ZF|PF|CF);
    if (isnan(a) || isnan(b)) c->eflags |= ZF|PF|CF;
    else if (a < b)           c->eflags |= CF;
    else if (a == b)          c->eflags |= ZF;
    c->lf_size = 0;
}
static void jit_x87_fxam( struct x86cpu *c )
{
    double v=fp_get(c,0); int cls=fpclassify(v), cc=0;
    /* FXAM: C3,C2,C0 classify ST0; C1 reports its sign.  The interpreter
     * stores the x87 stack as doubles, so the IEEE-754 classes are the
     * available representation of normal, denormal, zero, infinity, NaN. */
    if(cls==FP_NAN) cc=0x0100;             /* NaN: 001 */
    else if(cls==FP_INFINITE) cc=0x0500;   /* infinity: 011 */
    else if(cls==FP_ZERO) cc=0x4000;       /* zero: 100 */
    else if(cls==FP_SUBNORMAL) cc=0x4200;  /* denormal: 110 */
    else cc=0x0400;                        /* normal: 010 */
    c->fpsw=(c->fpsw&~0x4700)|cc|(signbit(v)?0x0200:0);
}
static void jit_x87_math( struct x86cpu *c, int op )
{
    double x=fp_get(c,0);
    switch(op){
    case 0: fp_set(c,0,exp2(x)-1.0); break;       /* f2xm1 */
    case 1: fp_set(c,0,ldexp(x,(int)fp_get(c,1))); break; /* fscale */
    case 2: fp_set(c,0,fmod(x,fp_get(c,1))); break; /* fprem/fprem1 */
    case 3: fp_set(c,0,sin(x)); break;
    case 4: fp_set(c,0,cos(x)); break;
    }
}

/* forward: the native seam that runs when the guest calls a dispatcher */
extern int wasm_x86_dispatch( struct x86cpu *c, uint32_t target );

static struct x86cpu g_cpu;
struct x86cpu *wasm_x86_current(void) { return &g_cpu; }

static void unimplemented( struct x86cpu *c, uint32_t eip, uint8_t op )
{
    fprintf( stderr, "wasm_x86: UNIMPLEMENTED opcode %02x at eip=%08x bytes=%02x %02x %02x %02x %02x\n",
             op, eip, rd8(eip), rd8(eip+1), rd8(eip+2), rd8(eip+3), rd8(eip+4) );
    c->running = 0;
    c->exit_code = 0xE0000001;
}

/* Instruction-prefix classifier: nonzero for the legacy prefix bytes so the
 * decode loop's common no-prefix path is a single table lookup.
 * 1=66 2=67 3=f2/f3 4=64 5=65 6=26/2e/36/3e 7=f0. */
static const uint8_t prefix_class[256] = {
    [0x66]=1, [0x67]=2, [0xf2]=3, [0xf3]=3, [0x64]=4, [0x65]=5,
    [0x26]=6, [0x2e]=6, [0x36]=6, [0x3e]=6, [0xf0]=7,
};

/* SSE/SSE2/MMX plus the bit-test and cmpxchg/xadd tail of the 0x0f map.
 * Split out of run() for the same reason as run_x87: this is ~300 lines of
 * wide-vector code (8- and 16-lane loops) that netduke32's integer software
 * renderer almost never executes, yet its bulk sat inside the one function
 * every guest instruction dispatches through.  Returns 1 if handled, 0 if the
 * opcode is unknown (caller reports it).  Pure code motion. */
static __attribute__((noinline)) int run_sse( struct x86cpu *c, struct decode *d, uint8_t op2 )
{
    struct modrm m;
    uint32_t a, b, r;
    int os = d->opsize;
    (void)r;
    switch (op2)
    {
            case 0x10: case 0x28: /* movups/movaps/movss/movsd xmm, xmm/m */
                m=decode_modrm(c,d);
                { int scalar=(op2==0x10)&&(d->rep==0xf3||d->rep==0xf2);
                  int sz=(op2==0x10&&d->rep==0xf3)?4:(op2==0x10&&d->rep==0xf2)?8:16;
                  if (m.is_reg) memcpy(c->xmm[m.reg], c->xmm[m.rm], scalar?sz:16); /* reg movss/sd preserves upper */
                  else { if(scalar) memset(c->xmm[m.reg],0,16); /* mem movss/sd ZERO the upper lanes */
                         memcpy(c->xmm[m.reg], (void*)(uintptr_t)m.ea, sz); } }
                break;
            case 0x11: case 0x29: /* store */
                m=decode_modrm(c,d);
                { int scalar=(op2==0x11)&&(d->rep==0xf3||d->rep==0xf2);
                  int sz=(op2==0x11&&d->rep==0xf3)?4:(op2==0x11&&d->rep==0xf2)?8:16;
                  if (m.is_reg) memcpy(c->xmm[m.rm], c->xmm[m.reg], scalar?sz:16); /* reg movss/sd preserves upper */
                  else memcpy((void*)(uintptr_t)m.ea, c->xmm[m.reg], sz); }
                break;
            case 0x6f: /* movdqa/movdqu load */
                m=decode_modrm(c,d);
                if (m.is_reg) memcpy(c->xmm[m.reg], c->xmm[m.rm], 16);
                else memcpy(c->xmm[m.reg], (void*)(uintptr_t)m.ea, 16);
                break;
            case 0x7f: /* movdqa/movdqu store */
                m=decode_modrm(c,d);
                if (m.is_reg) memcpy(c->xmm[m.rm], c->xmm[m.reg], 16);
                else memcpy((void*)(uintptr_t)m.ea, c->xmm[m.reg], 16);
                break;
            case 0x6e: /* movd xmm, r/m32 */
                m=decode_modrm(c,d); memset(c->xmm[m.reg],0,16);
                { uint32_t v=read_rm(c,&m,4); memcpy(c->xmm[m.reg],&v,4); } break;
            case 0x7e: /* f3: movq xmm,xmm/m64 ; else movd r/m32,xmm */
                m=decode_modrm(c,d);
                if (d->rep==0xf3) { memset(c->xmm[m.reg],0,16);
                    if (m.is_reg) memcpy(c->xmm[m.reg], c->xmm[m.rm], 8); else memcpy(c->xmm[m.reg],(void*)(uintptr_t)m.ea,8); }
                else { uint32_t v; memcpy(&v,c->xmm[m.reg],4); write_rm(c,&m,4,v); }
                break;
            case 0xd6: /* movq store */
                m=decode_modrm(c,d);
                if (m.is_reg) { memset(c->xmm[m.rm],0,16); memcpy(c->xmm[m.rm], c->xmm[m.reg], 8); }
                else memcpy((void*)(uintptr_t)m.ea, c->xmm[m.reg], 8);
                break;
            case 0xef: /* pxor */
                m=decode_modrm(c,d);
                { uint8_t *src = m.is_reg ? c->xmm[m.rm] : (uint8_t*)(uintptr_t)m.ea; int i; for(i=0;i<16;i++) c->xmm[m.reg][i]^=src[i]; }
                break;
            case 0x74: case 0x75: case 0x76: /* pcmpeqb/w/d */
                m=decode_modrm(c,d);
                { uint8_t *src = m.is_reg ? c->xmm[m.rm] : (uint8_t*)(uintptr_t)m.ea; int i;
                  int step=(op2==0x74)?1:(op2==0x75)?2:4;
                  for(i=0;i<16;i+=step){ int eq=!memcmp(&c->xmm[m.reg][i],&src[i],step); memset(&c->xmm[m.reg][i],eq?0xff:0,step);} }
                break;
            case 0xd7: /* pmovmskb r32, xmm */
                m=decode_modrm(c,d);
                { uint32_t mask=0; int i; for(i=0;i<16;i++) if(c->xmm[m.rm][i]&0x80) mask|=(1u<<i); write_reg(c,m.reg,4,mask); }
                break;
            case 0xfc: case 0xfd: case 0xfe: /* paddb/w/d */
                m=decode_modrm(c,d);
                { uint8_t *src = m.is_reg ? c->xmm[m.rm] : (uint8_t*)(uintptr_t)m.ea; int i;
                  if(op2==0xfc) for(i=0;i<16;i++) c->xmm[m.reg][i]+=src[i];
                  else if(op2==0xfd){ uint16_t *a16=(uint16_t*)c->xmm[m.reg],*s16=(uint16_t*)src; for(i=0;i<8;i++) a16[i]+=s16[i]; }
                  else { uint32_t *a32=(uint32_t*)c->xmm[m.reg],*s32=(uint32_t*)src; for(i=0;i<4;i++) a32[i]+=s32[i]; } }
                break;
            case 0x12: case 0x13: case 0x16: case 0x17: /* movlps/movhps (mem) + movhlps/movlhps (reg) */
                m=decode_modrm(c,d);
                if (op2==0x12) { /* movlps m64->lo  OR  movhlps: dst.lo = src.hi */
                    if (m.is_reg) memcpy(c->xmm[m.reg], c->xmm[m.rm]+8, 8);
                    else memcpy(c->xmm[m.reg], (void*)(uintptr_t)m.ea, 8);
                }
                else if (op2==0x16) { /* movhps m64->hi  OR  movlhps: dst.hi = src.lo */
                    if (m.is_reg) memcpy(c->xmm[m.reg]+8, c->xmm[m.rm], 8);
                    else memcpy(c->xmm[m.reg]+8, (void*)(uintptr_t)m.ea, 8);
                }
                else if (op2==0x13) { if(!m.is_reg) memcpy((void*)(uintptr_t)m.ea, c->xmm[m.reg], 8); }      /* movlps store */
                else { if(!m.is_reg) memcpy((void*)(uintptr_t)m.ea, c->xmm[m.reg]+8, 8); }                    /* movhps store */
                break;
            case 0x57: /* xorps */
                m=decode_modrm(c,d);
                { uint8_t *src = m.is_reg ? c->xmm[m.rm] : (uint8_t*)(uintptr_t)m.ea; int i; for(i=0;i<16;i++) c->xmm[m.reg][i]^=src[i]; }
                break;
            case 0x55: /* andnps */
                m=decode_modrm(c,d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; int i; for(i=0;i<16;i++) c->xmm[m.reg][i]=(~c->xmm[m.reg][i])&s[i]; } break;
            /* --- SSE/SSE2 float arithmetic: prefix picks ps/pd/ss/sd --- */
            case 0x51: case 0x52: case 0x53: /* sqrt / rsqrt / rcp (unary on src) */
            case 0x58: case 0x59: case 0x5c: case 0x5d: case 0x5e: case 0x5f: /* add mul sub min div max */
            {
                m=decode_modrm(c,d); uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint8_t *dst=c->xmm[m.reg];
                int dbl=(d->opsize==2||d->rep==0xf2), scal=(d->rep==0xf3||d->rep==0xf2);
                int lanes=dbl?2:4, n=scal?1:lanes, i;
                for(i=0;i<n;i++){
                    double x,y,r;
                    if(dbl){ uint64_t xb,yb; memcpy(&xb,dst+i*8,8); memcpy(&yb,s+i*8,8); memcpy(&x,&xb,8); memcpy(&y,&yb,8); }
                    else { float xf,yf; memcpy(&xf,dst+i*4,4); memcpy(&yf,s+i*4,4); x=xf; y=yf; }
                    switch(op2){
                        case 0x51: r=sqrt(y); break; case 0x52: r=1.0/sqrt(y); break; case 0x53: r=1.0/y; break;
                        case 0x58: r=x+y; break; case 0x59: r=x*y; break; case 0x5c: r=x-y; break;
                        case 0x5d: r=(x<y)?x:y; break; case 0x5e: r=x/y; break; case 0x5f: r=(x>y)?x:y; break;
                        default: r=x;
                    }
                    if(dbl){ uint64_t rb; memcpy(&rb,&r,8); memcpy(dst+i*8,&rb,8); }
                    else { float rf=(float)r; memcpy(dst+i*4,&rf,4); }
                }
                break;
            }
            case 0x2a: /* cvtsi2ss/sd (int r/m32 -> scalar float in dst low) */
                m=decode_modrm(c,d); { int32_t v=(int32_t)read_rm(c,&m,4); uint8_t *dst=c->xmm[m.reg];
                    if(d->rep==0xf2){ double dv=v; memcpy(dst,&dv,8); } else { float fv=(float)v; memcpy(dst,&fv,4); } } break;
            case 0x2c: case 0x2d: /* cvt(t)ss2si / sd2si -> r32 */
                m=decode_modrm(c,d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; double v;
                    if(d->rep==0xf2){ uint64_t b; memcpy(&b,s,8); memcpy(&v,&b,8); } else { float f; memcpy(&f,s,4); v=f; }
                    int32_t r=(op2==0x2c)?(int32_t)v:(int32_t)llrint(v); write_reg(c,m.reg,4,(uint32_t)r); } break;
            case 0x2e: case 0x2f: /* ucomiss/comiss (66 -> sd) -> EFLAGS */
                m=decode_modrm(c,d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; double x,y;
                    if(d->opsize==2){ uint64_t xb,yb; memcpy(&xb,c->xmm[m.reg],8); memcpy(&yb,s,8); memcpy(&x,&xb,8); memcpy(&y,&yb,8); }
                    else { float xf,yf; memcpy(&xf,c->xmm[m.reg],4); memcpy(&yf,s,4); x=xf; y=yf; }
                    c->eflags &= ~(ZF|PF|CF|OF|SF|AF); c->lf_size=0;
                    if(isnan(x)||isnan(y)) c->eflags|=ZF|PF|CF; else if(x<y) c->eflags|=CF; else if(x==y) c->eflags|=ZF; }
                break;
            case 0x5a: /* cvtps2pd/pd2ps/ss2sd/sd2ss */
                m=decode_modrm(c,d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint8_t *dst=c->xmm[m.reg];
                    if(d->rep==0xf3){ float f; memcpy(&f,s,4); double dv=f; memcpy(dst,&dv,8); }
                    else if(d->rep==0xf2){ uint64_t b; double dv; memcpy(&b,s,8); memcpy(&dv,&b,8); float f=(float)dv; memcpy(dst,&f,4); }
                    else if(d->opsize==2){ uint64_t b0,b1; double d0,d1; memcpy(&b0,s,8); memcpy(&b1,s+8,8); memcpy(&d0,&b0,8); memcpy(&d1,&b1,8); float f0=(float)d0,f1=(float)d1; uint8_t t[16]; memset(t,0,16); memcpy(t,&f0,4); memcpy(t+4,&f1,4); memcpy(dst,t,16); }
                    else { float f0,f1; memcpy(&f0,s,4); memcpy(&f1,s+4,4); double d0=f0,d1=f1; memcpy(dst,&d0,8); memcpy(dst+8,&d1,8); } }
                break;
            case 0x5b: /* cvtdq2ps / cvtps2dq(66) / cvttps2dq(f3) */
                m=decode_modrm(c,d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint8_t *dst=c->xmm[m.reg]; uint8_t t[16]; int i;
                    if(d->rep==0xf3){ for(i=0;i<4;i++){ float f; memcpy(&f,s+i*4,4); int32_t v=(int32_t)f; memcpy(t+i*4,&v,4);} }
                    else if(d->opsize==2){ for(i=0;i<4;i++){ float f; memcpy(&f,s+i*4,4); int32_t v=(int32_t)llrintf(f); memcpy(t+i*4,&v,4);} }
                    else { for(i=0;i<4;i++){ int32_t v; memcpy(&v,s+i*4,4); float f=(float)v; memcpy(t+i*4,&f,4);} }
                    memcpy(dst,t,16); }
                break;
            case 0xe6: /* cvtdq2pd(f3) / cvttpd2dq(66) / cvtpd2dq(f2) */
                m=decode_modrm(c,d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint8_t *dst=c->xmm[m.reg]; uint8_t t[16]; memset(t,0,16); int i;
                    if(d->rep==0xf3){ for(i=0;i<2;i++){ int32_t v; memcpy(&v,s+i*4,4); double dv=v; memcpy(t+i*8,&dv,8);} }
                    else { int trunc=(d->opsize==2); for(i=0;i<2;i++){ uint64_t b; double dv; memcpy(&b,s+i*8,8); memcpy(&dv,&b,8); int32_t v=trunc?(int32_t)dv:(int32_t)llrint(dv); memcpy(t+i*4,&v,4);} }
                    memcpy(dst,t,16); }
                break;
            case 0xc2: /* cmpps/pd/ss/sd imm8 predicate */
                m=decode_modrm(c,d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint8_t *dst=c->xmm[m.reg]; uint8_t im=f8(d);
                    int dbl=(d->opsize==2||d->rep==0xf2), scal=(d->rep==0xf3||d->rep==0xf2); int lanes=dbl?2:4, n=scal?1:lanes, i;
                    for(i=0;i<n;i++){ double x,y; if(dbl){ uint64_t xb,yb; memcpy(&xb,dst+i*8,8); memcpy(&yb,s+i*8,8); memcpy(&x,&xb,8); memcpy(&y,&yb,8);} else { float xf,yf; memcpy(&xf,dst+i*4,4); memcpy(&yf,s+i*4,4); x=xf; y=yf; }
                        int un=isnan(x)||isnan(y),res; switch(im&7){ case 0:res=(x==y);break; case 1:res=(x<y);break; case 2:res=(x<=y);break; case 3:res=un;break; case 4:res=!(x==y);break; case 5:res=!(x<y);break; case 6:res=!(x<=y);break; default:res=!un; }
                        if(dbl){ uint64_t mm=res?~0ULL:0; memcpy(dst+i*8,&mm,8);} else { uint32_t mm=res?~0U:0; memcpy(dst+i*4,&mm,4);} } }
                break;
            case 0xc6: /* shufps / shufpd(66) imm8 */
                m=decode_modrm(c,d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint8_t *dst=c->xmm[m.reg]; uint8_t im=f8(d); uint8_t t[16];
                    if(d->opsize==2){ memcpy(&t[0],&dst[(im&1)?8:0],8); memcpy(&t[8],&s[(im&2)?8:0],8); }
                    else { uint32_t *td=(uint32_t*)t,*dd=(uint32_t*)dst,*sd=(uint32_t*)s; td[0]=dd[(im>>0)&3]; td[1]=dd[(im>>2)&3]; td[2]=sd[(im>>4)&3]; td[3]=sd[(im>>6)&3]; }
                    memcpy(dst,t,16); }
                break;
            case 0xdb: /* pand */ m=decode_modrm(c,d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; int i; for(i=0;i<16;i++) c->xmm[m.reg][i]&=s[i]; } break;
            case 0xdf: /* pandn */ m=decode_modrm(c,d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; int i; for(i=0;i<16;i++) c->xmm[m.reg][i]=(~c->xmm[m.reg][i])&s[i]; } break;
            case 0xeb: /* por */ m=decode_modrm(c,d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; int i; for(i=0;i<16;i++) c->xmm[m.reg][i]|=s[i]; } break;
            case 0x60: case 0x61: case 0x62: case 0x6c: /* punpckl bw/wd/dq/qdq */
            {
                m=decode_modrm(c,d); uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint8_t tmp[16]; uint8_t *dst=c->xmm[m.reg];
                int u = op2==0x60?1:op2==0x61?2:op2==0x62?4:8; int i,o=0;
                for (i=0;i<8/ (u? u:1) + (u==8?1:0); i++) {} /* interleave low half */
                { int k; o=0; for(k=0;k<8;k+=u){ memcpy(&tmp[o],&dst[k],u); o+=u; memcpy(&tmp[o],&s[k],u); o+=u; if(o>=16)break;} }
                memcpy(dst,tmp,16); break;
            }
            case 0x68: case 0x69: case 0x6a: case 0x6d: /* punpckh */
            {
                m=decode_modrm(c,d); uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint8_t tmp[16]; uint8_t *dst=c->xmm[m.reg];
                int u = op2==0x68?1:op2==0x69?2:op2==0x6a?4:8; int k,o=0;
                for(k=8;k<16;k+=u){ memcpy(&tmp[o],&dst[k],u); o+=u; memcpy(&tmp[o],&s[k],u); o+=u; if(o>=16)break;}
                memcpy(dst,tmp,16); break;
            }
            case 0x70: /* pshufd(66)/pshuflw(f2)/pshufhw(f3) */
            {
                m=decode_modrm(c,d); uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint8_t im=f8(d); uint8_t tmp[16];
                if (d->opsize==2) { uint32_t *sd=(uint32_t*)s,*td=(uint32_t*)tmp; int i; for(i=0;i<4;i++) td[i]=sd[(im>>(i*2))&3]; }
                else if (d->rep==0xf2) { memcpy(tmp,s,16); uint16_t *sw=(uint16_t*)s,*tw=(uint16_t*)tmp; int i; for(i=0;i<4;i++) tw[i]=sw[(im>>(i*2))&3]; }
                else { memcpy(tmp,s,16); uint16_t *sw=(uint16_t*)s,*tw=(uint16_t*)tmp; int i; for(i=0;i<4;i++) tw[4+i]=sw[4+((im>>(i*2))&3)]; }
                memcpy(c->xmm[m.reg],tmp,16); break;
            }
            case 0x71: case 0x72: case 0x73: /* psll/psrl/psra by imm8 (reg field selects op) */
            {
                m=decode_modrm(c,d); uint8_t im=f8(d); uint8_t *dst=c->xmm[m.rm];
                if (op2==0x73 && (m.reg==7||m.reg==3)) { /* pslldq/psrldq (byte shift) */
                    uint8_t tmp[16]; memset(tmp,0,16);
                    if(m.reg==7){ int sh=im; if(sh<16) memcpy(tmp+sh,dst,16-sh); } /* pslldq */
                    else { int sh=im; if(sh<16) memcpy(tmp,dst+sh,16-sh); } /* psrldq */
                    memcpy(dst,tmp,16); break;
                }
                int u = op2==0x71?2:4; if(op2==0x73&&m.reg!=2&&m.reg!=6) u=8;
                int n=16/u,i;
                for(i=0;i<n;i++){ uint64_t v=0; memcpy(&v,&dst[i*u],u);
                    if(m.reg==6) v<<=im; else if(m.reg==2) v>>=im; else { /* psra */ int64_t sv=(int64_t)(v<<(64-u*8)); sv>>=(64-u*8); v=(uint64_t)(sv>>im);}
                    memcpy(&dst[i*u],&v,u);} break;
            }
            case 0xc5: /* pextrw r32, xmm, imm8 */
                m=decode_modrm(c,d); { uint8_t im=f8(d); uint16_t *w=(uint16_t*)c->xmm[m.rm]; write_reg(c,m.reg,4,w[im&7]); } break;
            case 0xc4: /* pinsrw xmm, r/m16, imm8 */
                m=decode_modrm(c,d); { uint32_t v=read_rm(c,&m,2); uint8_t im=f8(d); uint16_t *w=(uint16_t*)c->xmm[m.reg]; w[im&7]=(uint16_t)v; } break;
            case 0x50: /* movmskps r32, xmm */
                m=decode_modrm(c,d); { uint32_t mask=0; int i; for(i=0;i<4;i++) if(c->xmm[m.rm][i*4+3]&0x80) mask|=(1u<<i); write_reg(c,m.reg,4,mask); } break;
            case 0x64: case 0x65: case 0x66: /* pcmpgtb/w/d (signed) */
                m=decode_modrm(c,d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint8_t *dst=c->xmm[m.reg]; int i;
                  if(op2==0x64){ for(i=0;i<16;i++) dst[i]=((int8_t)dst[i]>(int8_t)s[i])?0xff:0; }
                  else if(op2==0x65){ int16_t *a=(int16_t*)dst,*b=(int16_t*)s; for(i=0;i<8;i++) a[i]=(a[i]>b[i])?-1:0; }
                  else { int32_t *a=(int32_t*)dst,*b=(int32_t*)s; for(i=0;i<4;i++) a[i]=(a[i]>b[i])?-1:0; } } break;
            case 0xd1: case 0xd2: case 0xd3: /* psrlw/d/q by xmm */
            case 0xf1: case 0xf2: case 0xf3: /* psllw/d/q by xmm */
            case 0xe1: case 0xe2: /* psraw/psrad by xmm */
                m=decode_modrm(c,d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint8_t *dst=c->xmm[m.reg];
                  uint64_t cnt; memcpy(&cnt,s,8);
                  int u=(op2==0xd1||op2==0xf1||op2==0xe1)?2:(op2==0xd3||op2==0xf3)?8:4;
                  int arith=(op2==0xe1||op2==0xe2), left=(op2==0xf1||op2==0xf2||op2==0xf3);
                  int n=16/u,i; for(i=0;i<n;i++){ uint64_t v=0; memcpy(&v,&dst[i*u],u); int bits=u*8;
                    if(cnt>=(uint64_t)bits){ if(arith){ int64_t sv=(int64_t)(v<<(64-bits)); sv>>=63; v=(uint64_t)sv; } else v=0; }
                    else if(left) v<<=cnt;
                    else if(arith){ int64_t sv=(int64_t)(v<<(64-bits)); sv>>=(64-bits); v=(uint64_t)(sv>>cnt); }
                    else v>>=cnt;
                    memcpy(&dst[i*u],&v,u);} } break;
            case 0xd4: /* paddq */ m=decode_modrm(c,d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint64_t *a=(uint64_t*)c->xmm[m.reg],*b=(uint64_t*)s; a[0]+=b[0]; a[1]+=b[1]; } break;
            case 0xfb: /* psubq */ m=decode_modrm(c,d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint64_t *a=(uint64_t*)c->xmm[m.reg],*b=(uint64_t*)s; a[0]-=b[0]; a[1]-=b[1]; } break;
            case 0xd5: /* pmullw */ m=decode_modrm(c,d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint16_t *a=(uint16_t*)c->xmm[m.reg],*b=(uint16_t*)s; int i; for(i=0;i<8;i++) a[i]=(uint16_t)(a[i]*b[i]); } break;
            case 0xe5: /* pmulhw */ m=decode_modrm(c,d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; int16_t *a=(int16_t*)c->xmm[m.reg],*b=(int16_t*)s; int i; for(i=0;i<8;i++) a[i]=(int16_t)(((int32_t)a[i]*b[i])>>16); } break;
            case 0xe4: /* pmulhuw */ m=decode_modrm(c,d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint16_t *a=(uint16_t*)c->xmm[m.reg],*b=(uint16_t*)s; int i; for(i=0;i<8;i++) a[i]=(uint16_t)(((uint32_t)a[i]*b[i])>>16); } break;
            case 0xf4: /* pmuludq */ m=decode_modrm(c,d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint32_t *a=(uint32_t*)c->xmm[m.reg],*b=(uint32_t*)s; uint64_t r0=(uint64_t)a[0]*b[0], r1=(uint64_t)a[2]*b[2]; uint64_t *o=(uint64_t*)c->xmm[m.reg]; o[0]=r0; o[1]=r1; } break;
            case 0xf5: /* pmaddwd */ m=decode_modrm(c,d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; int16_t *a=(int16_t*)c->xmm[m.reg],*b=(int16_t*)s; int32_t r[4]; int i; for(i=0;i<4;i++) r[i]=(int32_t)a[i*2]*b[i*2]+(int32_t)a[i*2+1]*b[i*2+1]; memcpy(c->xmm[m.reg],r,16); } break;
            case 0xf6: /* psadbw */ m=decode_modrm(c,d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint8_t *dst=c->xmm[m.reg]; uint32_t s0=0,s1=0; int i; for(i=0;i<8;i++){ int dd=dst[i]-s[i]; s0+=dd<0?-dd:dd; } for(i=8;i<16;i++){ int dd=dst[i]-s[i]; s1+=dd<0?-dd:dd; } uint64_t *o=(uint64_t*)dst; o[0]=s0; o[1]=s1; } break;
            case 0xd8: case 0xd9: /* psubusb/psubusw */ m=decode_modrm(c,d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint8_t *dst=c->xmm[m.reg]; int i;
                  if(op2==0xd8){ for(i=0;i<16;i++){ int r=dst[i]-s[i]; dst[i]=r<0?0:r; } }
                  else { uint16_t *a=(uint16_t*)dst,*b=(uint16_t*)s; for(i=0;i<8;i++){ int r=a[i]-b[i]; a[i]=r<0?0:(uint16_t)r; } } } break;
            case 0xdc: case 0xdd: /* paddusb/paddusw */ m=decode_modrm(c,d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint8_t *dst=c->xmm[m.reg]; int i;
                  if(op2==0xdc){ for(i=0;i<16;i++){ int r=dst[i]+s[i]; dst[i]=r>255?255:r; } }
                  else { uint16_t *a=(uint16_t*)dst,*b=(uint16_t*)s; for(i=0;i<8;i++){ int r=a[i]+b[i]; a[i]=r>65535?65535:(uint16_t)r; } } } break;
            case 0xe8: case 0xe9: /* psubsb/psubsw */ m=decode_modrm(c,d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint8_t *dst=c->xmm[m.reg]; int i;
                  if(op2==0xe8){ for(i=0;i<16;i++){ int r=(int8_t)dst[i]-(int8_t)s[i]; dst[i]=(uint8_t)(int8_t)(r<-128?-128:r>127?127:r); } }
                  else { int16_t *a=(int16_t*)dst,*b=(int16_t*)s; for(i=0;i<8;i++){ int r=a[i]-b[i]; a[i]=(int16_t)(r<-32768?-32768:r>32767?32767:r); } } } break;
            case 0xec: case 0xed: /* paddsb/paddsw */ m=decode_modrm(c,d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint8_t *dst=c->xmm[m.reg]; int i;
                  if(op2==0xec){ for(i=0;i<16;i++){ int r=(int8_t)dst[i]+(int8_t)s[i]; dst[i]=(uint8_t)(int8_t)(r<-128?-128:r>127?127:r); } }
                  else { int16_t *a=(int16_t*)dst,*b=(int16_t*)s; for(i=0;i<8;i++){ int r=a[i]+b[i]; a[i]=(int16_t)(r<-32768?-32768:r>32767?32767:r); } } } break;
            case 0xda: case 0xde: /* pminub/pmaxub */ m=decode_modrm(c,d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint8_t *dst=c->xmm[m.reg]; int i;
                  if(op2==0xda){ for(i=0;i<16;i++) if(s[i]<dst[i]) dst[i]=s[i]; } else { for(i=0;i<16;i++) if(s[i]>dst[i]) dst[i]=s[i]; } } break;
            case 0xea: case 0xee: /* pminsw/pmaxsw */ m=decode_modrm(c,d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; int16_t *a=(int16_t*)c->xmm[m.reg],*b=(int16_t*)s; int i;
                  if(op2==0xea){ for(i=0;i<8;i++) if(b[i]<a[i]) a[i]=b[i]; } else { for(i=0;i<8;i++) if(b[i]>a[i]) a[i]=b[i]; } } break;
            case 0xe0: case 0xe3: /* pavgb/pavgw */ m=decode_modrm(c,d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint8_t *dst=c->xmm[m.reg]; int i;
                  if(op2==0xe0){ for(i=0;i<16;i++) dst[i]=(uint8_t)((dst[i]+s[i]+1)>>1); } else { uint16_t *a=(uint16_t*)dst,*b=(uint16_t*)s; for(i=0;i<8;i++) a[i]=(uint16_t)((a[i]+b[i]+1)>>1); } } break;
            case 0xf8: case 0xf9: case 0xfa: /* psubb/w/d */ m=decode_modrm(c,d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint8_t *dst=c->xmm[m.reg]; int i;
                  if(op2==0xf8){ for(i=0;i<16;i++) dst[i]-=s[i]; }
                  else if(op2==0xf9){ uint16_t *a=(uint16_t*)dst,*b=(uint16_t*)s; for(i=0;i<8;i++) a[i]-=b[i]; }
                  else { uint32_t *a=(uint32_t*)dst,*b=(uint32_t*)s; for(i=0;i<4;i++) a[i]-=b[i]; } } break;
            case 0x54: /* andps */ m=decode_modrm(c,d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; int i; for(i=0;i<16;i++) c->xmm[m.reg][i]&=s[i]; } break;
            case 0x56: /* orps */ m=decode_modrm(c,d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; int i; for(i=0;i<16;i++) c->xmm[m.reg][i]|=s[i]; } break;
            case 0x14: case 0x15: /* unpcklps/unpckhps — 4-byte lane interleave */
                m=decode_modrm(c,d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint32_t *dd=(uint32_t*)c->xmm[m.reg],*ss=(uint32_t*)s; uint32_t t[4]; int base=(op2==0x15)?2:0; t[0]=dd[base];t[1]=ss[base];t[2]=dd[base+1];t[3]=ss[base+1]; memcpy(dd,t,16);} break;
            case 0x77: break; /* emms */
            case 0xae: decode_modrm(c,d); break; /* fxsave/lfence/sfence/mfence group: ignore */
            case 0x18: case 0x19: case 0x1a: case 0x1b: case 0x1c: case 0x1d: case 0x1e:
                decode_modrm(c,d); break; /* prefetch/nop hints */
            case 0xa3: /* bt r/m, r */
                m=decode_modrm(c,d); a=read_rm(c,&m,os); b=read_reg(c,m.reg,os)&(os*8-1);
                c->eflags=(c->eflags&~CF)|(((a>>b)&1)?CF:0); c->lf_size=0; break;
            case 0xab: /* bts */
                m=decode_modrm(c,d); a=read_rm(c,&m,os); b=read_reg(c,m.reg,os)&(os*8-1);
                c->eflags=(c->eflags&~CF)|(((a>>b)&1)?CF:0); c->lf_size=0; write_rm(c,&m,os,a|(1u<<b)); break;
            case 0xb3: /* btr r/m, r */
                m=decode_modrm(c,d); a=read_rm(c,&m,os); b=read_reg(c,m.reg,os)&(os*8-1);
                c->eflags=(c->eflags&~CF)|(((a>>b)&1)?CF:0); c->lf_size=0; write_rm(c,&m,os,a&~(1u<<b)); break;
            case 0xbb: /* btc r/m, r */
                m=decode_modrm(c,d); a=read_rm(c,&m,os); b=read_reg(c,m.reg,os)&(os*8-1);
                c->eflags=(c->eflags&~CF)|(((a>>b)&1)?CF:0); c->lf_size=0; write_rm(c,&m,os,a^(1u<<b)); break;
            case 0xba: /* grp8 bt/bts/btr/btc r/m, imm8 */
                m=decode_modrm(c,d); a=read_rm(c,&m,os); { uint32_t bit=f8(d)&(os*8-1);
                  c->eflags=(c->eflags&~CF)|(((a>>bit)&1)?CF:0); c->lf_size=0;
                  if(m.reg==5) write_rm(c,&m,os,a|(1u<<bit)); else if(m.reg==6) write_rm(c,&m,os,a&~(1u<<bit)); else if(m.reg==7) write_rm(c,&m,os,a^(1u<<bit)); }
                break;
            case 0xb0: case 0xb1: /* cmpxchg */
            {
                int sz=(op2&1)?os:1; m=decode_modrm(c,d); a=read_rm(c,&m,sz); uint32_t acc=read_reg(c,EAX,sz);
                if (acc==a) { write_rm(c,&m,sz,read_reg(c,m.reg,sz)); set_lazy(c,K_SUB,acc,a,0,sz); }
                else { write_reg(c,EAX,sz,a); set_lazy(c,K_SUB,acc,a,acc-a,sz); }
                break;
            }
            case 0xc0: case 0xc1: /* xadd */
            {
                int sz=(op2&1)?os:1; m=decode_modrm(c,d); a=read_rm(c,&m,sz); b=read_reg(c,m.reg,sz);
                write_rm(c,&m,sz,a+b); write_reg(c,m.reg,sz,a); set_lazy(c,K_ADD,a,b,a+b,sz); break;
            }
            case 0xc7: /* cmpxchg8b m64 (reg field 1) */
            {
                m = decode_modrm(c,d);
                if (m.reg == 1) {
                    uint64_t mem = rd32(m.ea) | ((uint64_t)rd32(m.ea+4) << 32);
                    uint64_t edxeax = c->regs[EAX] | ((uint64_t)c->regs[EDX] << 32);
                    c->eflags = get_flags(c);           /* materialize; only ZF changes */
                    if (mem == edxeax) {
                        wr32(m.ea, c->regs[EBX]); wr32(m.ea+4, c->regs[ECX]);
                        c->eflags |= ZF;
                    } else {
                        c->regs[EAX] = (uint32_t)mem; c->regs[EDX] = (uint32_t)(mem >> 32);
                        c->eflags &= ~ZF;
                    }
                    c->lf_size = 0;
                }
                break;
            }
            case 0xc8: case 0xc9: case 0xca: case 0xcb: case 0xcc: case 0xcd: case 0xce: case 0xcf: /* bswap */
                { int rr=op2&7; uint32_t v=c->regs[rr]; c->regs[rr]=((v>>24)&0xff)|((v>>8)&0xff00)|((v<<8)&0xff0000)|((v<<24)&0xff000000); } break;
            case 0xbc: /* bsf */ m=decode_modrm(c,d); a=read_rm(c,&m,os); if(a){ int i=0; while(!((a>>i)&1))i++; write_reg(c,m.reg,os,i); c->eflags&=~ZF;} else c->eflags|=ZF; c->lf_size=0; break;
            case 0xbd: /* bsr */ m=decode_modrm(c,d); a=read_rm(c,&m,os); if(a){ int i=os*8-1; while(!((a>>i)&1))i--; write_reg(c,m.reg,os,i); c->eflags&=~ZF;} else c->eflags|=ZF; c->lf_size=0; break;
            case 0xa4: case 0xa5: /* SHLD r/m, r, imm8/cl */
            case 0xac: case 0xad: /* SHRD r/m, r, imm8/cl */
            {
                int left = (op2==0xa4||op2==0xa5);
                m=decode_modrm(c,d);
                uint32_t dst=read_rm(c,&m,os), src=read_reg(c,m.reg,os);
                uint32_t cnt = (op2==0xa4||op2==0xac) ? f8(d) : (c->regs[ECX]&0xff);
                cnt &= 31;
                int W = os*8;
                if (cnt == 0) { write_rm(c,&m,os,dst & sizemask(os)); break; }
                if (cnt >= (uint32_t)W) cnt %= (uint32_t)W;       /* 16-bit UB guard */
                if (cnt == 0) { write_rm(c,&m,os,dst & sizemask(os)); break; }
                uint32_t mask=sizemask(os), sm=signmask(os), res, cf;
                dst &= mask; src &= mask;
                if (left) { res = ((dst<<cnt) | (src>>(W-cnt))) & mask; cf = (dst >> (W-cnt)) & 1; }
                else      { res = ((dst>>cnt) | (src<<(W-cnt))) & mask; cf = (dst >> (cnt-1)) & 1; }
                write_rm(c,&m,os,res);
                { uint32_t f = c->eflags & ~(CF|PF|AF|ZF|SF|OF);
                  if (cf) f|=CF;
                  if (!(res & mask)) f|=ZF;
                  if (res & sm) f|=SF;
                  if (parity((uint8_t)res)) f|=PF;
                  if (cnt==1 && ((res ^ dst) & sm)) f|=OF;
                  c->eflags = f; c->lf_size = 0; }
                break;
            }
            case 0x1f: decode_modrm(c,d); break; /* nop r/m */
            case 0x31: /* rdtsc */ c->regs[EAX]=0; c->regs[EDX]=0; break;
            case 0xa2: /* cpuid */ c->regs[EAX]=0; c->regs[EBX]=0; c->regs[ECX]=0; c->regs[EDX]=0; break;
    default: return 0;
    }
    return 1;
}

/* grp3 (F6/F7: test/not/neg/mul/imul/div/idiv) and the string instructions.
 * Split out of run() for the same reason as run_x87/run_sse: none of these
 * reach the top-16 opcode histogram, but the 64-bit divides and the rep loops
 * compile to a lot of code, and run() is register-pressure bound - every byte
 * of cold code in that one function costs the hot dispatch path.  Pure code
 * motion. */
/* grp3 mul/imul/div/idiv (F6/F7 with reg>=4).  Split from the hot test/not/neg
 * arms, which run() handles inline: 64-bit division compiles to a lot of code
 * and is rare, while "test" is in the engine's hottest per-frame loop.
 * The caller has already decoded the modrm. */
static __attribute__((noinline)) void run_cold_grp3( struct x86cpu *c, struct decode *d,
                                                     uint8_t op, struct modrm *mp, int sz )
{
    struct modrm m = *mp;
    uint32_t a = read_rm( c, &m, sz );
    (void)op; (void)d;
    switch (m.reg)
    {
    case 4: /* mul */ { uint64_t p=(uint64_t)(a&sizemask(sz))*(sz==1?(c->regs[EAX]&0xff):sz==2?(c->regs[EAX]&0xffff):c->regs[EAX]);
                        if(sz==1) c->regs[EAX]=(c->regs[EAX]&0xffff0000)|(uint16_t)p;
                        else if(sz==2){write_reg(c,EAX,2,(uint16_t)p); write_reg(c,EDX,2,(uint16_t)(p>>16));}
                        else {c->regs[EAX]=(uint32_t)p; c->regs[EDX]=(uint32_t)(p>>32);} } break;
    case 5: /* imul */ { int64_t p=(int64_t)(int32_t)a*(int32_t)c->regs[EAX];
                         if(sz==4){c->regs[EAX]=(uint32_t)p; c->regs[EDX]=(uint32_t)(p>>32);} else write_reg(c,EAX,sz,(uint32_t)p);} break;
    case 6: /* div */ if(sz==4){ uint64_t dividend=((uint64_t)c->regs[EDX]<<32)|c->regs[EAX]; if(a){c->regs[EAX]=(uint32_t)(dividend/a); c->regs[EDX]=(uint32_t)(dividend%a);} }
                      else if(sz==2){ uint32_t dividend=((c->regs[EDX]&0xffff)<<16)|(c->regs[EAX]&0xffff); if(a){write_reg(c,EAX,2,dividend/a); write_reg(c,EDX,2,dividend%a);} }
                      else { uint32_t dividend=c->regs[EAX]&0xffff; if(a){c->regs[EAX]=(c->regs[EAX]&0xffff0000)|((dividend/a)&0xff)|(((dividend%a)&0xff)<<8);} } break;
    case 7: /* idiv */ if(sz==4){ int64_t dividend=((int64_t)c->regs[EDX]<<32)|c->regs[EAX]; if(a){c->regs[EAX]=(uint32_t)(dividend/(int32_t)a); c->regs[EDX]=(uint32_t)(dividend%(int32_t)a);} } break;
    default: break;
    }
}

static __attribute__((noinline)) void run_cold( struct x86cpu *c, struct decode *d, uint8_t op )
{
    struct modrm m;
    uint32_t a, b, r;
    int os = d->opsize;
    (void)r;
    switch (op)
    {
        /* grp3 mul/div now live in run_cold_grp3 (below); test/not/neg are inline
         * in run() because they are hot. */
        case 0xf6: case 0xf7:
        {
            int sz=(op&1)?os:1;
            m=decode_modrm(c,d);
            a=read_rm(c,&m,sz);
            switch (m.reg)
            {
            case 0: case 1: b=(sz==1)?f8(d):(os==2?f16(d):f32(d)); set_lazy(c,K_LOGIC,a,b,a&b,sz); break; /* test */
            case 2: write_rm(c,&m,sz,~a); break; /* not */
            case 3: r=(uint32_t)(-(int32_t)a); write_rm(c,&m,sz,r); set_lazy(c,K_SUB,0,a,r,sz); break; /* neg */
            case 4: /* mul */ { uint64_t p=(uint64_t)(a&sizemask(sz))*(sz==1?(c->regs[EAX]&0xff):sz==2?(c->regs[EAX]&0xffff):c->regs[EAX]);
                                if(sz==1) c->regs[EAX]=(c->regs[EAX]&0xffff0000)|(uint16_t)p;
                                else if(sz==2){write_reg(c,EAX,2,(uint16_t)p); write_reg(c,EDX,2,(uint16_t)(p>>16));}
                                else {c->regs[EAX]=(uint32_t)p; c->regs[EDX]=(uint32_t)(p>>32);} } break;
            case 5: /* imul */ { int64_t p=(int64_t)(int32_t)a*(int32_t)c->regs[EAX];
                                 if(sz==4){c->regs[EAX]=(uint32_t)p; c->regs[EDX]=(uint32_t)(p>>32);} else write_reg(c,EAX,sz,(uint32_t)p);} break;
            case 6: /* div */ if(sz==4){ uint64_t dividend=((uint64_t)c->regs[EDX]<<32)|c->regs[EAX]; if(a){c->regs[EAX]=(uint32_t)(dividend/a); c->regs[EDX]=(uint32_t)(dividend%a);} }
                              else if(sz==2){ uint32_t dividend=((c->regs[EDX]&0xffff)<<16)|(c->regs[EAX]&0xffff); if(a){write_reg(c,EAX,2,dividend/a); write_reg(c,EDX,2,dividend%a);} }
                              else { uint32_t dividend=c->regs[EAX]&0xffff; if(a){c->regs[EAX]=(c->regs[EAX]&0xffff0000)|((dividend/a)&0xff)|(((dividend%a)&0xff)<<8);} } break;
            case 7: /* idiv */ if(sz==4){ int64_t dividend=((int64_t)c->regs[EDX]<<32)|c->regs[EAX]; if(a){c->regs[EAX]=(uint32_t)(dividend/(int32_t)a); c->regs[EDX]=(uint32_t)(dividend%(int32_t)a);} } break;
            }
            break;
        }

        /* string ops (addr32) with optional rep */
        case 0xa4: case 0xa5: /* movs */
        {
            int sz=(op&1)?os:1; uint32_t cnt=(d->rep)?c->regs[ECX]:1; int delta=(c->eflags&DF)?-sz:sz;
            while (cnt--) { uint32_t v=(sz==1)?rd8(c->regs[ESI]):sz==2?rd16(c->regs[ESI]):rd32(c->regs[ESI]);
                            if(sz==1)wr8(c->regs[EDI],v); else if(sz==2)wr16(c->regs[EDI],v); else wr32(c->regs[EDI],v);
                            c->regs[ESI]+=delta; c->regs[EDI]+=delta; }
            if (d->rep) c->regs[ECX]=0;
            break;
        }
        case 0xaa: case 0xab: /* stos */
        {
            int sz=(op&1)?os:1; uint32_t cnt=(d->rep)?c->regs[ECX]:1; int delta=(c->eflags&DF)?-sz:sz; uint32_t v=c->regs[EAX];
            while (cnt--) { if(sz==1)wr8(c->regs[EDI],v); else if(sz==2)wr16(c->regs[EDI],v); else wr32(c->regs[EDI],v); c->regs[EDI]+=delta; }
            if (d->rep) c->regs[ECX]=0;
            break;
        }
        case 0xac: case 0xad: /* lods */
        {
            int sz=(op&1)?os:1; int delta=(c->eflags&DF)?-sz:sz;
            uint32_t v=(sz==1)?rd8(c->regs[ESI]):sz==2?rd16(c->regs[ESI]):rd32(c->regs[ESI]);
            write_reg(c,EAX,sz,v); c->regs[ESI]+=delta;
            break;
        }
        case 0xc0: case 0xc1: case 0xd0: case 0xd1: case 0xd2: case 0xd3:
        {
            int sz = (op&1)?os:1;
            m=decode_modrm(c,d);
            uint32_t cnt;
            if (op==0xc0||op==0xc1) cnt=f8(d);
            else if (op==0xd0||op==0xd1) cnt=1;
            else cnt=c->regs[ECX]&0xff;
            cnt &= 31;
            a=read_rm(c,&m,sz);
            uint32_t sm=signmask(sz), mask=sizemask(sz);
            a &= mask;
            if (cnt == 0) { write_rm(c,&m,sz,a); break; }  /* x86: count 0 leaves flags */
            { int W = sz*8; uint32_t cf=0, of=0; int rotate=0;
              switch (m.reg)
              {
              case 4: case 6: /* shl/sal */
                  cf = (cnt <= (uint32_t)W) ? ((a >> (W-cnt)) & 1) : 0;
                  r = (a << cnt) & mask;
                  of = ((r & sm)?1:0) ^ cf;
                  break;
              case 5: /* shr */
                  cf = (a >> (cnt-1)) & 1;
                  r = a >> cnt;
                  of = (a & sm)?1:0;
                  break;
              case 7: /* sar */
                  { int32_t sa=(int32_t)(a<<(32-W)); sa>>=(32-W);
                    cf = (a >> (cnt-1)) & 1;
                    r = ((uint32_t)(sa >> cnt)) & mask; of = 0; }
                  break;
              case 0: /* rol */
                  { uint32_t n = cnt % (uint32_t)W; r = n ? (((a<<n)|(a>>(W-n)))&mask) : a;
                    cf = r & 1; of = ((r & sm)?1:0) ^ cf; rotate=1; }
                  break;
              case 1: /* ror */
                  { uint32_t n = cnt % (uint32_t)W; r = n ? (((a>>n)|(a<<(W-n)))&mask) : a;
                    cf = (r >> (W-1)) & 1; of = ((r & sm)?1:0) ^ (((r>>(W-2))&1)); rotate=1; }
                  break;
              default: r = (a<<cnt)&mask; write_rm(c,&m,sz,r); c->lf_size=0; break;
              }
              if (m.reg <= 1 || (m.reg >= 4 && m.reg <= 7))
              {
                  write_rm(c,&m,sz,r);
                  if (rotate) { /* rotates affect only CF/OF; preserve SF/ZF/PF */
                      c->eflags = get_flags(c);
                      c->eflags = (c->eflags & ~(CF|OF)) | (cf?CF:0) | (of?OF:0);
                  } else {
                      uint32_t f = c->eflags & ~(CF|PF|AF|ZF|SF|OF);
                      if (cf) f|=CF; if (of) f|=OF;
                      if (!(r & mask)) f|=ZF;
                      if (r & sm) f|=SF;
                      if (parity((uint8_t)r)) f|=PF;
                      c->eflags = f;
                  }
                  c->lf_size = 0;
              }
            }
            break;
        }
        case 0x10: { int ci=lf_cf(c); m=decode_modrm(c,d); a=read_rm(c,&m,1); b=read_reg(c,m.reg,1); r=a+b+ci; write_rm(c,&m,1,r); set_lazy(c,K_ADC,a,b,r,1); c->lf_cin=ci; } break;
        case 0x11: { int ci=lf_cf(c); m=decode_modrm(c,d); a=read_rm(c,&m,os); b=read_reg(c,m.reg,os); r=a+b+ci; write_rm(c,&m,os,r); set_lazy(c,K_ADC,a,b,r,os); c->lf_cin=ci; } break;
        case 0x13: { int ci=lf_cf(c); m=decode_modrm(c,d); a=read_reg(c,m.reg,os); b=read_rm(c,&m,os); r=a+b+ci; write_reg(c,m.reg,os,r); set_lazy(c,K_ADC,a,b,r,os); c->lf_cin=ci; } break;
        case 0x18: { int ci=lf_cf(c); m=decode_modrm(c,d); a=read_rm(c,&m,1); b=read_reg(c,m.reg,1); r=a-b-ci; write_rm(c,&m,1,r); set_lazy(c,K_SBB,a,b,r,1); c->lf_cin=ci; } break;
        case 0x19: { int ci=lf_cf(c); m=decode_modrm(c,d); a=read_rm(c,&m,os); b=read_reg(c,m.reg,os); r=a-b-ci; write_rm(c,&m,os,r); set_lazy(c,K_SBB,a,b,r,os); c->lf_cin=ci; } break;
        case 0x1b: { int ci=lf_cf(c); m=decode_modrm(c,d); a=read_reg(c,m.reg,os); b=read_rm(c,&m,os); r=a-b-ci; write_reg(c,m.reg,os,r); set_lazy(c,K_SBB,a,b,r,os); c->lf_cin=ci; } break;
        case 0x12: { int ci=lf_cf(c); m=decode_modrm(c,d); a=read_reg(c,m.reg,1); b=read_rm(c,&m,1); r=a+b+ci; write_reg(c,m.reg,1,r); set_lazy(c,K_ADC,a,b,r,1); c->lf_cin=ci; } break; /* adc r8,r/m8 */
        case 0x14: { int ci=lf_cf(c); a=read_reg(c,EAX,1); b=f8(d); r=a+b+ci; write_reg(c,EAX,1,r); set_lazy(c,K_ADC,a,b,r,1); c->lf_cin=ci; } break; /* adc al,imm8 */
        case 0x15: { int ci=lf_cf(c); a=read_reg(c,EAX,os); b=os==2?f16(d):f32(d); r=a+b+ci; write_reg(c,EAX,os,r); set_lazy(c,K_ADC,a,b,r,os); c->lf_cin=ci; } break; /* adc eax,imm */
        case 0x1a: { int ci=lf_cf(c); m=decode_modrm(c,d); a=read_reg(c,m.reg,1); b=read_rm(c,&m,1); r=a-b-ci; write_reg(c,m.reg,1,r); set_lazy(c,K_SBB,a,b,r,1); c->lf_cin=ci; } break; /* sbb r8,r/m8 */
        case 0x1c: { int ci=lf_cf(c); a=read_reg(c,EAX,1); b=f8(d); r=a-b-ci; write_reg(c,EAX,1,r); set_lazy(c,K_SBB,a,b,r,1); c->lf_cin=ci; } break; /* sbb al,imm8 */
        case 0x1d: { int ci=lf_cf(c); a=read_reg(c,EAX,os); b=os==2?f16(d):f32(d); r=a-b-ci; write_reg(c,EAX,os,r); set_lazy(c,K_SBB,a,b,r,os); c->lf_cin=ci; } break; /* sbb eax,imm */
    default: break;
    }
}

/* x87 FPU (0xd8-0xdf).  Split out of run() so the interpreter's hot integer
 * path stays a small function: run() compiled to ~76KB of wasm in ONE body,
 * which is 90% of the module and well past the point where the engine's
 * optimizer allocates registers well.  This code is cold (x87 does not appear
 * in the top-16 opcode histogram) but its bulk penalised every instruction.
 * Pure code motion - semantics are unchanged. */
static __attribute__((noinline)) void run_x87( struct x86cpu *c, struct decode *d, uint8_t op )
{
    struct modrm m;
    switch (op)
    {
        case 0xd8: /* arithmetic with m32real / st(i), dest st0 */
        {
            m = decode_modrm(c,d);
            double src = m.is_reg ? fp_get(c,m.rm) : rdf32(m.ea);
            double s0 = fp_get(c,0);
            switch (m.reg) {
            case 0: fp_set(c,0,s0+src); break;
            case 1: fp_set(c,0,s0*src); break;
            case 2: fp_compare(c,s0,src); break;
            case 3: fp_compare(c,s0,src); fp_pop(c); break;
            case 4: fp_set(c,0,s0-src); break;
            case 5: fp_set(c,0,src-s0); break;
            case 6: fp_set(c,0,s0/src); break;
            case 7: fp_set(c,0,src/s0); break;
            }
            break;
        }
        case 0xd9:
        {
            m = decode_modrm(c,d);
            if (!m.is_reg) {
                switch (m.reg) {
                case 0: fp_push(c, rdf32(m.ea)); break;        /* fld m32 */
                case 2: wrf32(m.ea, fp_get(c,0)); break;       /* fst m32 */
                case 3: wrf32(m.ea, fp_pop(c)); break;         /* fstp m32 */
                case 5: c->fpcw = rd16(m.ea); break;           /* fldcw */
                case 7: wr16(m.ea, c->fpcw); break;            /* fnstcw */
                default: break;                                /* fldenv/fnstenv: skip */
                }
            } else {
                int b = 0xc0 | (m.reg<<3) | m.rm;
                if (m.reg == 0) { double v=fp_get(c,m.rm); fp_push(c,v); }   /* fld st(i) */
                else if (m.reg == 1) { double t=fp_get(c,0); fp_set(c,0,fp_get(c,m.rm)); fp_set(c,m.rm,t); } /* fxch */
                else switch (b) {
                    case 0xe0: fp_set(c,0,-fp_get(c,0)); break;        /* fchs */
                    case 0xe1: fp_set(c,0,fabs(fp_get(c,0))); break;   /* fabs */
                    case 0xe4: fp_compare(c,fp_get(c,0),0.0); break;   /* ftst */
                    case 0xe5: break;                                  /* fxam: skip */
                    case 0xe8: fp_push(c,1.0); break;                  /* fld1 */
                    case 0xe9: fp_push(c,3.321928094887362); break;    /* fldl2t */
                    case 0xea: fp_push(c,1.4426950408889634); break;   /* fldl2e */
                    case 0xeb: fp_push(c,3.141592653589793); break;    /* fldpi */
                    case 0xec: fp_push(c,0.3010299956639812); break;   /* fldlg2 */
                    case 0xed: fp_push(c,0.6931471805599453); break;   /* fldln2 */
                    case 0xee: fp_push(c,0.0); break;                  /* fldz */
                    case 0xf0: fp_set(c,0,exp2(fp_get(c,0))-1.0); break; /* f2xm1 */
                    case 0xf1: fp_set(c,1,fp_get(c,1)*log2(fp_get(c,0))); fp_pop(c); break; /* fyl2x */
                    case 0xf2: { double t=tan(fp_get(c,0)); fp_set(c,0,t); fp_push(c,1.0); } break; /* fptan */
                    case 0xf3: fp_set(c,1,atan2(fp_get(c,1),fp_get(c,0))); fp_pop(c); break; /* fpatan */
                    case 0xf5: fp_set(c,0,fmod(fp_get(c,0),fp_get(c,1))); break; /* fprem1 */
                    case 0xf6: c->fptop=(c->fptop-1)&7; break;         /* fdecstp */
                    case 0xf7: c->fptop=(c->fptop+1)&7; break;         /* fincstp */
                    case 0xf8: fp_set(c,0,fmod(fp_get(c,0),fp_get(c,1))); break; /* fprem */
                    case 0xfa: fp_set(c,0,sqrt(fp_get(c,0))); break;   /* fsqrt */
                    case 0xfb: { double s=sin(fp_get(c,0)),co=cos(fp_get(c,0)); fp_set(c,0,s); fp_push(c,co); } break; /* fsincos */
                    case 0xfc: fp_set(c,0,rint(fp_get(c,0))); break;   /* frndint */
                    case 0xfd: fp_set(c,0,ldexp(fp_get(c,0),(int)fp_get(c,1))); break; /* fscale */
                    case 0xfe: fp_set(c,0,sin(fp_get(c,0))); break;    /* fsin */
                    case 0xff: fp_set(c,0,cos(fp_get(c,0))); break;    /* fcos */
                    default: break;
                }
            }
            break;
        }
        case 0xda:
        {
            m = decode_modrm(c,d);
            if (!m.is_reg) {
                double src = (double)(int32_t)rd32(m.ea), s0 = fp_get(c,0);
                switch (m.reg) {
                case 0: fp_set(c,0,s0+src); break; case 1: fp_set(c,0,s0*src); break;
                case 2: fp_compare(c,s0,src); break; case 3: fp_compare(c,s0,src); fp_pop(c); break;
                case 4: fp_set(c,0,s0-src); break; case 5: fp_set(c,0,src-s0); break;
                case 6: fp_set(c,0,s0/src); break; case 7: fp_set(c,0,src/s0); break;
                }
            } else {
                int b = 0xc0 | (m.reg<<3) | m.rm;
                uint32_t f = get_flags(c);
                if (b == 0xe9) { fp_compare(c,fp_get(c,0),fp_get(c,1)); fp_pop(c); fp_pop(c); } /* fucompp */
                else { int mv=0; switch(m.reg){case 0:mv=!!(f&CF);break;case 1:mv=!!(f&ZF);break;case 2:mv=!!(f&(CF|ZF));break;case 3:mv=!!(f&PF);break;}
                       if (mv) fp_set(c,0,fp_get(c,m.rm)); } /* fcmovb/e/be/u */
            }
            break;
        }
        case 0xdb:
        {
            m = decode_modrm(c,d);
            if (!m.is_reg) {
                switch (m.reg) {
                case 0: fp_push(c,(double)(int32_t)rd32(m.ea)); break;     /* fild m32 */
                case 2: wr32(m.ea,(uint32_t)(int32_t)lrint(fp_get(c,0))); break; /* fist m32 */
                case 3: wr32(m.ea,(uint32_t)(int32_t)lrint(fp_pop(c))); break;   /* fistp m32 */
                case 5: fp_push(c, rd80(m.ea)); break;                     /* fld m80 */
                case 7: wr80(m.ea, fp_pop(c)); break;                      /* fstp m80 */
                default: break;
                }
            } else {
                int b = 0xc0 | (m.reg<<3) | m.rm;
                uint32_t f = get_flags(c);
                if (b == 0xe2) { c->fpsw &= ~0xff; }                       /* fnclex */
                else if (b == 0xe3) { c->fpcw=0x037f; c->fpsw=0; c->fptop=0; } /* fninit */
                else if (b >= 0xe8 && b <= 0xf7) fp_compare_eflags(c,fp_get(c,0),fp_get(c,m.rm)); /* fucomi/fcomi */
                else { int mv=0; switch(m.reg){case 0:mv=!(f&CF);break;case 1:mv=!(f&ZF);break;case 2:mv=!(f&(CF|ZF));break;case 3:mv=!(f&PF);break;}
                       if (mv) fp_set(c,0,fp_get(c,m.rm)); } /* fcmovnb/ne/nbe/nu */
            }
            break;
        }
        case 0xdc:
        {
            m = decode_modrm(c,d);
            if (!m.is_reg) {
                double src = rdf64(m.ea), s0 = fp_get(c,0);
                switch (m.reg) {
                case 0: fp_set(c,0,s0+src); break; case 1: fp_set(c,0,s0*src); break;
                case 2: fp_compare(c,s0,src); break; case 3: fp_compare(c,s0,src); fp_pop(c); break;
                case 4: fp_set(c,0,s0-src); break; case 5: fp_set(c,0,src-s0); break;
                case 6: fp_set(c,0,s0/src); break; case 7: fp_set(c,0,src/s0); break;
                }
            } else {
                double si = fp_get(c,m.rm), s0 = fp_get(c,0);  /* dest st(i); sub/div reversed */
                switch (m.reg) {
                case 0: fp_set(c,m.rm,si+s0); break; case 1: fp_set(c,m.rm,si*s0); break;
                case 4: fp_set(c,m.rm,s0-si); break; case 5: fp_set(c,m.rm,si-s0); break;
                case 6: fp_set(c,m.rm,s0/si); break; case 7: fp_set(c,m.rm,si/s0); break;
                default: break;
                }
            }
            break;
        }
        case 0xdd:
        {
            m = decode_modrm(c,d);
            if (!m.is_reg) {
                switch (m.reg) {
                case 0: fp_push(c, rdf64(m.ea)); break;        /* fld m64 */
                case 2: wrf64(m.ea, fp_get(c,0)); break;       /* fst m64 */
                case 3: wrf64(m.ea, fp_pop(c)); break;         /* fstp m64 */
                case 7: wr16(m.ea, (uint16_t)(c->fpsw | ((c->fptop&7)<<11))); break; /* fnstsw m16 */
                default: break;
                }
            } else {
                switch (m.reg) {
                case 0: break;                                 /* ffree: no-op */
                case 2: fp_set(c,m.rm,fp_get(c,0)); break;      /* fst st(i) */
                case 3: { double v=fp_get(c,0); fp_set(c,m.rm,v); fp_pop(c); } break; /* fstp st(i) */
                case 4: fp_compare(c,fp_get(c,0),fp_get(c,m.rm)); break;       /* fucom */
                case 5: fp_compare(c,fp_get(c,0),fp_get(c,m.rm)); fp_pop(c); break; /* fucomp */
                default: break;
                }
            }
            break;
        }
        case 0xde:
        {
            m = decode_modrm(c,d);
            if (!m.is_reg) {
                double src = (double)(int16_t)rd16(m.ea), s0 = fp_get(c,0);
                switch (m.reg) {
                case 0: fp_set(c,0,s0+src); break; case 1: fp_set(c,0,s0*src); break;
                case 2: fp_compare(c,s0,src); break; case 3: fp_compare(c,s0,src); fp_pop(c); break;
                case 4: fp_set(c,0,s0-src); break; case 5: fp_set(c,0,src-s0); break;
                case 6: fp_set(c,0,s0/src); break; case 7: fp_set(c,0,src/s0); break;
                }
            } else {
                int b = 0xc0 | (m.reg<<3) | m.rm;
                if (b == 0xd9) { fp_compare(c,fp_get(c,0),fp_get(c,1)); fp_pop(c); fp_pop(c); } /* fcompp */
                else {
                    double si = fp_get(c,m.rm), s0 = fp_get(c,0), r = 0;   /* st(i) op= st0; pop */
                    switch (m.reg) {
                    case 0: r=si+s0; break; case 1: r=si*s0; break;
                    case 4: r=s0-si; break; case 5: r=si-s0; break;
                    case 6: r=s0/si; break; case 7: r=si/s0; break;
                    default: r=si; break;
                    }
                    fp_set(c,m.rm,r); fp_pop(c);
                }
            }
            break;
        }
        case 0xdf:
        {
            m = decode_modrm(c,d);
            if (!m.is_reg) {
                switch (m.reg) {
                case 0: fp_push(c,(double)(int16_t)rd16(m.ea)); break;     /* fild m16 */
                case 2: wr16(m.ea,(uint16_t)(int16_t)lrint(fp_get(c,0))); break; /* fist m16 */
                case 3: wr16(m.ea,(uint16_t)(int16_t)lrint(fp_pop(c))); break;   /* fistp m16 */
                case 5: { int64_t v=(int64_t)(rd32(m.ea)|((uint64_t)rd32(m.ea+4)<<32)); fp_push(c,(double)v); } break; /* fild m64 */
                case 7: { int64_t v=(int64_t)llrint(fp_pop(c)); wr32(m.ea,(uint32_t)v); wr32(m.ea+4,(uint32_t)((uint64_t)v>>32)); } break; /* fistp m64 */
                default: break;
                }
            } else {
                int b = 0xc0 | (m.reg<<3) | m.rm;
                if (b == 0xe0) write_reg(c,EAX,2,(uint16_t)(c->fpsw | ((c->fptop&7)<<11))); /* fnstsw ax */
                else if (b >= 0xe8 && b <= 0xf7) { fp_compare_eflags(c,fp_get(c,0),fp_get(c,m.rm)); fp_pop(c); } /* fucomip/fcomip */
                else fp_pop(c);                                            /* ffreep */
            }
            break;
        }

    default: break;
    }
}

/* Execute until the guest transfers to a native dispatcher (returns 1, target
 * left in c->eip) or stops. */
/* ---- AOT-translated basic blocks (hybrid recompiler) ---------------------
 * gen_blocks.c is x86toc.py's static translation of the guest's hot .text into
 * C basic-block functions that call the SAME helpers as the interpreter (same
 * module -> direct calls, no JIT boundary tax).  A block runs a whole straight
 * line of guest instructions in one call, then sets c->eip and returns; the
 * interpreter is the fallback for anything not translated (unhandled opcode,
 * SMC, indirect target with no block).  Gated on WASM_JIT + the compile flag. */
#ifdef WEBWINE_GENBLOCKS
/* Shift/rotate helper shared with the AOT-translated blocks: replicates the
 * interpreter's 0xc0-0xd3 flag semantics exactly (the verify pass enforces the
 * match).  regf is the modrm.reg field: 4/6=shl/sal 5=shr 7=sar 0=rol 1=ror. */
static uint32_t do_shift( struct x86cpu *c, int regf, uint32_t a, uint32_t cnt, int sz )
{
    uint32_t sm = signmask(sz), mask = sizemask(sz);
    cnt &= 31; a &= mask;
    if (cnt == 0) return a;                      /* x86: count 0 leaves flags */
    int W = sz*8; uint32_t r, cf=0, of=0; int rotate=0;
    switch (regf)
    {
    case 4: case 6:
        cf = (cnt <= (uint32_t)W) ? ((a >> (W-cnt)) & 1) : 0;
        r = (a << cnt) & mask;
        of = ((r & sm)?1:0) ^ cf; break;
    case 5:
        cf = (a >> (cnt-1)) & 1; r = a >> cnt; of = (a & sm)?1:0; break;
    case 7:
        { int32_t sa=(int32_t)(a<<(32-W)); sa>>=(32-W);
          cf = (a >> (cnt-1)) & 1; r = ((uint32_t)(sa >> cnt)) & mask; of = 0; } break;
    case 0:
        { uint32_t n = cnt % (uint32_t)W; r = n ? (((a<<n)|(a>>(W-n)))&mask) : a;
          cf = r & 1; of = ((r & sm)?1:0) ^ cf; rotate=1; } break;
    case 1:
        { uint32_t n = cnt % (uint32_t)W; r = n ? (((a>>n)|(a<<(W-n)))&mask) : a;
          cf = (r >> (W-1)) & 1; of = ((r & sm)?1:0) ^ (((r>>(W-2))&1)); rotate=1; } break;
    default: c->lf_size=0; return (a<<cnt)&mask;
    }
    if (rotate) {
        c->eflags = get_flags(c);
        c->eflags = (c->eflags & ~(CF|OF)) | (cf?CF:0) | (of?OF:0);
    } else {
        uint32_t f = c->eflags & ~(CF|PF|AF|ZF|SF|OF);
        if (cf) f|=CF; if (of) f|=OF;
        if (!(r & mask)) f|=ZF;
        if (r & sm) f|=SF;
        if (parity((uint8_t)r)) f|=PF;
        c->eflags = f;
    }
    c->lf_size = 0;
    return r;
}
/* SHLD/SHRD helper (0x0f a4/a5/ac/ad): double-precision shift. */
static uint32_t do_dshift( struct x86cpu *c, int left, uint32_t dst, uint32_t src, uint32_t cnt, int sz )
{
    uint32_t mask = sizemask(sz), sm = signmask(sz); int W = sz*8;
    cnt &= 31; dst &= mask; src &= mask;
    if (cnt == 0) return dst;
    if (cnt >= (uint32_t)W) cnt %= (uint32_t)W;      /* 16-bit UB guard */
    if (cnt == 0) return dst;
    uint32_t res, cf;
    if (left) { res = ((dst<<cnt) | (src>>(W-cnt))) & mask; cf = (dst >> (W-cnt)) & 1; }
    else      { res = ((dst>>cnt) | (src<<(W-cnt))) & mask; cf = (dst >> (cnt-1)) & 1; }
    uint32_t f = c->eflags & ~(CF|PF|AF|ZF|SF|OF);
    if (cf) f|=CF;
    if (!(res & mask)) f|=ZF;
    if (res & sm) f|=SF;
    if (parity((uint8_t)res)) f|=PF;
    if (cnt==1 && ((res ^ dst) & sm)) f|=OF;
    c->eflags = f; c->lf_size = 0;
    return res;
}
/* 1-operand mul/imul/div/idiv (F6/F7 reg 4-7): EDX:EAX result, no flags.
 * Mirrors run_cold_grp3 exactly.  regf is the modrm.reg field. */
static void do_muldiv( struct x86cpu *c, int regf, uint32_t a, int sz )
{
    switch (regf) {
    case 4: { uint64_t p=(uint64_t)(a&sizemask(sz))*(sz==1?(c->regs[EAX]&0xff):sz==2?(c->regs[EAX]&0xffff):c->regs[EAX]);
              if(sz==1) c->regs[EAX]=(c->regs[EAX]&0xffff0000)|(uint16_t)p;
              else if(sz==2){write_reg(c,EAX,2,(uint16_t)p); write_reg(c,EDX,2,(uint16_t)(p>>16));}
              else {c->regs[EAX]=(uint32_t)p; c->regs[EDX]=(uint32_t)(p>>32);} } break;
    case 5: { int64_t p=(int64_t)(int32_t)a*(int32_t)c->regs[EAX];
              if(sz==4){c->regs[EAX]=(uint32_t)p; c->regs[EDX]=(uint32_t)(p>>32);} else write_reg(c,EAX,sz,(uint32_t)p);} break;
    case 6: if(sz==4){ uint64_t dd=((uint64_t)c->regs[EDX]<<32)|c->regs[EAX]; if(a){c->regs[EAX]=(uint32_t)(dd/a); c->regs[EDX]=(uint32_t)(dd%a);} }
            else if(sz==2){ uint32_t dd=((c->regs[EDX]&0xffff)<<16)|(c->regs[EAX]&0xffff); if(a){write_reg(c,EAX,2,dd/a); write_reg(c,EDX,2,dd%a);} }
            else { uint32_t dd=c->regs[EAX]&0xffff; if(a){c->regs[EAX]=(c->regs[EAX]&0xffff0000)|((dd/a)&0xff)|(((dd%a)&0xff)<<8);} } break;
    case 7: if(sz==4){ int64_t dd=((int64_t)c->regs[EDX]<<32)|c->regs[EAX]; if(a){c->regs[EAX]=(uint32_t)(dd/(int32_t)a); c->regs[EDX]=(uint32_t)(dd%(int32_t)a);} } break;
    }
}
static int      g_jit_on, g_jit_verify, g_jit_filling, g_verify_budget;
/* Small XMM primitives used by the AOT translator.  Memory writes go through
 * wr8 so differential verification can undo them just like interpreter writes. */
static void jit_xmm_load( struct x86cpu *c, int dst, uint32_t a, int n, int zero )
{
    int i;
    if (zero) for (i=0;i<16;i++) c->xmm[dst][i]=0;
    for (i=0;i<n;i++) c->xmm[dst][i]=rd8(a+(uint32_t)i);
}
static void jit_xmm_store( struct x86cpu *c, int src, uint32_t a, int n )
{
    int i; for (i=0;i<n;i++) wr8(a+(uint32_t)i,c->xmm[src][i]);
}
static void jit_xmm_copy( struct x86cpu *c, int dst, int src, int n )
{
    uint8_t t[16]; memcpy(t,c->xmm[src],n); memcpy(c->xmm[dst],t,n);
}
static void jit_xmm_xor( struct x86cpu *c, int dst, int src )
{
    int i; for (i=0;i<16;i++) c->xmm[dst][i]^=c->xmm[src][i];
}
static void jit_xmm_unpcklo( struct x86cpu *c, int dst, int src )
{
    uint8_t d[16],s[16]; memcpy(d,c->xmm[dst],16); memcpy(s,c->xmm[src],16);
    memcpy(c->xmm[dst],d,4); memcpy(c->xmm[dst]+4,s,4);
    memcpy(c->xmm[dst]+8,d+4,4); memcpy(c->xmm[dst]+12,s+4,4);
}
static void jit_xmm_movehalf( struct x86cpu *c, int dst, int src, int high )
{
    uint8_t t[8]; int so=high?0:8,doff=high?8:0;
    memcpy(t,c->xmm[src]+so,8); memcpy(c->xmm[dst]+doff,t,8);
}
static void jit_xmm_binss( struct x86cpu *c, int dst, int src, uint32_t a,
                           int ismem, int op )
{
    float x,y,r; memcpy(&x,c->xmm[dst],4);
    if (ismem) { uint32_t v=rd32(a); memcpy(&y,&v,4); }
    else memcpy(&y,c->xmm[src],4);
    if (op==0) r=x+y; else if (op==1) r=x-y; else if (op==2) r=x*y; else r=x/y;
    memcpy(c->xmm[dst],&r,4);
}
static void jit_xmm_rsqrtss( struct x86cpu *c, int dst, int src, uint32_t a, int ismem )
{
    float x; uint32_t v; if(ismem){v=rd32(a);memcpy(&x,&v,4);}else memcpy(&x,c->xmm[src],4);
    x=1.0f/sqrtf(x); memcpy(c->xmm[dst],&x,4);
}
static void jit_xmm_loadhalf( struct x86cpu *c, int dst, uint32_t a, int high )
{
    int i,off=high?8:0; for(i=0;i<8;i++) c->xmm[dst][off+i]=rd8(a+(uint32_t)i);
}
static void jit_xmm_storehalf( struct x86cpu *c, int src, uint32_t a, int high )
{
    int i,off=high?8:0; for(i=0;i<8;i++) wr8(a+(uint32_t)i,c->xmm[src][off+i]);
}
static void jit_xmm_store16( struct x86cpu *c, int src, uint32_t a )
{ int i; for(i=0;i<16;i++) wr8(a+(uint32_t)i,c->xmm[src][i]); }
static void jit_xmm_cvtsi2ss( struct x86cpu *c, int dst, uint32_t v )
{
    float f=(float)(int32_t)v; memcpy(c->xmm[dst],&f,4);
}
static void jit_xmm_compare_ss( struct x86cpu *c, int dst, int src,
                                uint32_t a, int ismem )
{
    float x,y; uint32_t v;
    memcpy(&x,c->xmm[dst],4);
    if (ismem) { v=rd32(a); memcpy(&y,&v,4); } else memcpy(&y,c->xmm[src],4);
    c->eflags &= ~(ZF|PF|CF|OF|SF|AF); c->lf_size=0;
    if (isnan(x)||isnan(y)) c->eflags|=ZF|PF|CF;
    else if (x<y) c->eflags|=CF; else if (x==y) c->eflags|=ZF;
}
static void jit_xmm_cmpps( struct x86cpu *c, int dst, int src, uint32_t a,
                           int ismem, int pred, int scalar )
{
    uint8_t s[16],d[16],o[16];int i,n=scalar?1:4;memcpy(d,c->xmm[dst],16);for(i=0;i<16;i++)s[i]=ismem?rd8(a+(uint32_t)i):c->xmm[src][i];memcpy(o,d,16);
    for(i=0;i<n;i++){float x,y;int un,r;memcpy(&x,d+i*4,4);memcpy(&y,s+i*4,4);un=isnan(x)||isnan(y);switch(pred){case 0:r=x==y;break;case 1:r=x<y;break;case 2:r=x<=y;break;case 3:r=un;break;case 4:r=!(x==y);break;case 5:r=!(x<y);break;case 6:r=!(x<=y);break;default:r=!un;}uint32_t z=r?0xffffffffu:0;memcpy(o+i*4,&z,4);}memcpy(c->xmm[dst],o,16);
}
static void jit_xmm_logic( struct x86cpu *c, int dst, int src, uint32_t a,
                           int ismem, int op )
{
    int i; for(i=0;i<16;i++){ uint8_t s=ismem?rd8(a+(uint32_t)i):c->xmm[src][i]; if(op==0)c->xmm[dst][i]&=s; else if(op==1)c->xmm[dst][i]|=s; else if(op==2)c->xmm[dst][i]=(uint8_t)(~c->xmm[dst][i])&s; else c->xmm[dst][i]^=s; }
}
static int32_t jit_xmm_cvtss2si( struct x86cpu *c, int src, uint32_t a,
                                 int ismem, int truncv )
{
    float f; uint32_t v; if(ismem){v=rd32(a);memcpy(&f,&v,4);}else memcpy(&f,c->xmm[src],4);
    return truncv?(int32_t)f:(int32_t)lrintf(f);
}
static void jit_xmm_minmaxss( struct x86cpu *c, int dst, int src, uint32_t a,
                              int ismem, int ismax )
{
    float x,y,r; uint32_t v; memcpy(&x,c->xmm[dst],4);
    if(ismem){v=rd32(a);memcpy(&y,&v,4);}else memcpy(&y,c->xmm[src],4);
    r=ismax?((x>y)?x:y):((x<y)?x:y); memcpy(c->xmm[dst],&r,4);
}
static void jit_xmm_binps( struct x86cpu *c, int dst, int src, uint32_t a,
                           int ismem, int op )
{
    float x,y,r; uint8_t s[16]; int i; for(i=0;i<16;i++)s[i]=ismem?rd8(a+(uint32_t)i):c->xmm[src][i];
    for(i=0;i<4;i++){memcpy(&x,c->xmm[dst]+i*4,4);memcpy(&y,s+i*4,4);if(op==0)r=x+y;else if(op==1)r=x-y;else if(op==2)r=x*y;else r=x/y;memcpy(c->xmm[dst]+i*4,&r,4);}
}
static void jit_xmm_binps3( struct x86cpu *c, int dst, int src1, int src2,
                            uint32_t a, int ismem, int op )
{
    float x,y,r; uint8_t s[16]; int i; memcpy(s,c->xmm[src1],16);
    if(ismem) for(i=0;i<16;i++) s[ i ]=rd8(a+(uint32_t)i);
    for(i=0;i<4;i++){memcpy(&x,c->xmm[src1]+i*4,4);memcpy(&y,ismem?s+i*4:c->xmm[src2]+i*4,4);if(op==0)r=x+y;else if(op==1)r=x-y;else if(op==2)r=x*y;else r=x/y;memcpy(c->xmm[dst]+i*4,&r,4);}
}
static void jit_xmm_sqrt( struct x86cpu *c, int dst, int src )
{
    uint8_t s[16]; int i; memcpy(s,c->xmm[src],16); for(i=0;i<4;i++){float x,r;memcpy(&x,s+i*4,4);r=sqrtf(x);memcpy(c->xmm[dst]+i*4,&r,4);}
}
static void jit_xmm_movd_load( struct x86cpu *c, int dst, uint32_t v )
{
    int i; for(i=0;i<16;i++) c->xmm[dst][i]=0; memcpy(c->xmm[dst],&v,4);
}
static void jit_xmm_movd_store( struct x86cpu *c, int src, uint32_t a )
{
    uint32_t v; memcpy(&v,c->xmm[src],4); wr32(a,v);
}
static uint32_t jit_xmm_movd_value( struct x86cpu *c, int src )
{
    uint32_t v; memcpy(&v,c->xmm[src],4); return v;
}
static void jit_xmm_movq_load( struct x86cpu *c, int dst, const uint8_t *src )
{
    int i; for(i=0;i<16;i++) c->xmm[dst][i]=0; memcpy(c->xmm[dst],src,8);
}
static void jit_xmm_movq_store( struct x86cpu *c, int src, uint32_t a )
{
    int i; for(i=0;i<8;i++) wr8(a+(uint32_t)i,c->xmm[src][i]);
}
static void jit_mmx_movq_load( struct x86cpu *c, int dst, const uint8_t *src )
{
    int i; for(i=0;i<16;i++)c->xmm[dst][i]=0; memcpy(c->xmm[dst],src,8);
}
static void jit_mmx_movq_store( struct x86cpu *c, int src, uint32_t a )
{
    int i;for(i=0;i<8;i++)wr8(a+(uint32_t)i,c->xmm[src][i]);
}
static void jit_xmm_pinsrw( struct x86cpu *c, int dst, uint32_t v, uint32_t imm )
{ uint32_t i=imm&7; c->xmm[dst][i*2]=(uint8_t)v; c->xmm[dst][i*2+1]=(uint8_t)(v>>8); }
static void jit_xmm_psadbw( struct x86cpu *c, int dst, int src, uint32_t a, int ismem )
{
    uint8_t s[16],d[16],o[16]; int i,j; uint64_t sum;
    memcpy(d,c->xmm[dst],16); for(i=0;i<16;i++)s[i]=ismem?rd8(a+(uint32_t)i):c->xmm[src][i]; memset(o,0,16);
    for(j=0;j<2;j++){sum=0;for(i=0;i<8;i++)sum+=(d[j*8+i]>s[j*8+i])?d[j*8+i]-s[j*8+i]:s[j*8+i]-d[j*8+i];memcpy(o+j*8,&sum,8);} memcpy(c->xmm[dst],o,16);
}
static void jit_xmm_pmuludq( struct x86cpu *c, int dst, int src, uint32_t a, int ismem )
{
    uint8_t s[16],d[16],o[16]; uint32_t x,y; uint64_t z; int i; memcpy(d,c->xmm[dst],16);
    for(i=0;i<16;i++)s[i]=ismem?rd8(a+(uint32_t)i):c->xmm[src][i]; memset(o,0,16);
    for(i=0;i<2;i++){memcpy(&x,d+i*8,4);memcpy(&y,s+i*8,4);z=(uint64_t)x*y;memcpy(o+i*8,&z,8);} memcpy(c->xmm[dst],o,16);
}
static void jit_mmx_logic( struct x86cpu *c, int dst, int src, uint32_t a, int ismem, int op )
{
    int i;for(i=0;i<8;i++){uint8_t s=ismem?rd8(a+(uint32_t)i):c->xmm[src][i];if(op==0)c->xmm[dst][i]&=s;else if(op==1)c->xmm[dst][i]|=s;else if(op==2)c->xmm[dst][i]=(uint8_t)(~c->xmm[dst][i]&s);else c->xmm[dst][i]^=s;}
}
static void jit_mmx_pmullw( struct x86cpu *c, int dst, int src, uint32_t a, int ismem )
{
    uint8_t s[8];int i;for(i=0;i<8;i++)s[i]=ismem?rd8(a+(uint32_t)i):c->xmm[src][i];for(i=0;i<4;i++){int16_t x=(int16_t)((uint16_t)c->xmm[dst][2*i]|((uint16_t)c->xmm[dst][2*i+1]<<8)),y=(int16_t)((uint16_t)s[2*i]|((uint16_t)s[2*i+1]<<8));uint16_t z=(uint16_t)(x*y);c->xmm[dst][2*i]=(uint8_t)z;c->xmm[dst][2*i+1]=(uint8_t)(z>>8);}
}
static void jit_mmx_addsub( struct x86cpu *c, int dst, int src, uint32_t a, int ismem, int width, int sub )
{
    uint8_t s[8];int i;for(i=0;i<8;i++)s[i]=ismem?rd8(a+(uint32_t)i):c->xmm[src][i];
    if(width==1)for(i=0;i<8;i++)c->xmm[dst][i]=(uint8_t)(sub?c->xmm[dst][i]-s[i]:c->xmm[dst][i]+s[i]);
    else if(width==2)for(i=0;i<4;i++){uint16_t x=(uint16_t)c->xmm[dst][2*i]|((uint16_t)c->xmm[dst][2*i+1]<<8),y=(uint16_t)s[2*i]|((uint16_t)s[2*i+1]<<8),z=(uint16_t)(sub?x-y:x+y);c->xmm[dst][2*i]=(uint8_t)z;c->xmm[dst][2*i+1]=(uint8_t)(z>>8);}
    else{uint32_t x,y,z;for(i=0;i<2;i++){memcpy(&x,c->xmm[dst]+4*i,4);memcpy(&y,s+4*i,4);z=sub?x-y:x+y;memcpy(c->xmm[dst]+4*i,&z,4);}}
}
static void jit_mmx_pmulhw( struct x86cpu *c, int dst, int src, uint32_t a, int ismem )
{
    uint8_t s[8];int i;for(i=0;i<8;i++)s[i]=ismem?rd8(a+(uint32_t)i):c->xmm[src][i];for(i=0;i<4;i++){int16_t x=(int16_t)((uint16_t)c->xmm[dst][2*i]|((uint16_t)c->xmm[dst][2*i+1]<<8)),y=(int16_t)((uint16_t)s[2*i]|((uint16_t)s[2*i+1]<<8));int16_t z=(int16_t)(((int32_t)x*y)>>16);c->xmm[dst][2*i]=(uint8_t)z;c->xmm[dst][2*i+1]=(uint8_t)(z>>8);}
}
static void jit_mmx_satw( struct x86cpu *c, int dst, int src, uint32_t a, int ismem, int sub )
{ uint8_t s[8];int i;for(i=0;i<8;i++)s[i]=ismem?rd8(a+(uint32_t)i):c->xmm[src][i];for(i=0;i<4;i++){int x=(int16_t)((uint16_t)c->xmm[dst][2*i]|((uint16_t)c->xmm[dst][2*i+1]<<8)),y=(int16_t)((uint16_t)s[2*i]|((uint16_t)s[2*i+1]<<8)),z=sub?x-y:x+y;z=z<-32768?-32768:z>32767?32767:z;c->xmm[dst][2*i]=(uint8_t)z;c->xmm[dst][2*i+1]=(uint8_t)(z>>8);}}
static void jit_xmm_pmulhw( struct x86cpu *c, int dst, int src, uint32_t a, int ismem )
{ uint8_t s[16];int i;for(i=0;i<16;i++)s[i]=ismem?rd8(a+(uint32_t)i):c->xmm[src][i];for(i=0;i<8;i++){int16_t x=(int16_t)((uint16_t)c->xmm[dst][2*i]|((uint16_t)c->xmm[dst][2*i+1]<<8)),y=(int16_t)((uint16_t)s[2*i]|((uint16_t)s[2*i+1]<<8)),z=(int16_t)(((int32_t)x*y)>>16);c->xmm[dst][2*i]=(uint8_t)z;c->xmm[dst][2*i+1]=(uint8_t)(z>>8);}}
static void jit_mmx_unpck( struct x86cpu *c, int dst, int src, uint32_t a, int ismem, int width, int high )
{uint8_t d[8],s[8],o[8];int i,off=high?4:0,n=4/width;memcpy(d,c->xmm[dst],8);for(i=0;i<8;i++)s[i]=ismem?rd8(a+(uint32_t)i):c->xmm[src][i];for(i=0;i<n;i++){memcpy(o+i*2*width,d+off+i*width,width);memcpy(o+(i*2+1)*width,s+off+i*width,width);}memcpy(c->xmm[dst],o,8);}
static uint32_t jit_mmx_count( struct x86cpu *c, int src )
{ uint32_t v;memcpy(&v,c->xmm[src],4);return v; }
static void jit_mmx_shift( struct x86cpu *c, int dst, uint32_t count, int width, int arith, int left )
{
    int i,n=8/width,bits=width*8;if(count>=(uint32_t)bits){for(i=0;i<n;i++){int neg=arith&&!left&&(c->xmm[dst][i*width+width-1]&0x80);memset(c->xmm[dst]+i*width,neg?0xff:0,width);}return;}
    for(i=0;i<n;i++){uint64_t v=0;memcpy(&v,c->xmm[dst]+i*width,width);if(left)v<<=count;else if(arith){int64_t s=(int64_t)(v<<(64-bits));s>>=(64-bits);v=(uint64_t)(s>>count);}else v>>=count;memcpy(c->xmm[dst]+i*width,&v,width);}
}
static void jit_mmx_cmp( struct x86cpu *c, int dst, int src, uint32_t a, int ismem, int width, int gt )
{
    uint8_t s[8],d[8];int i;memcpy(d,c->xmm[dst],8);for(i=0;i<8;i++)s[i]=ismem?rd8(a+(uint32_t)i):c->xmm[src][i];for(i=0;i<8;i+=width){int64_t x=width==1?(int8_t)d[i]:(int16_t)((uint16_t)d[i]|((uint16_t)d[i+1]<<8)),y=width==1?(int8_t)s[i]:(int16_t)((uint16_t)s[i]|((uint16_t)s[i+1]<<8));if(width==4){x=(int32_t)((uint32_t)d[i]|((uint32_t)d[i+1]<<8)|((uint32_t)d[i+2]<<16)|((uint32_t)d[i+3]<<24));y=(int32_t)((uint32_t)s[i]|((uint32_t)s[i+1]<<8)|((uint32_t)s[i+2]<<16)|((uint32_t)s[i+3]<<24));}memset(c->xmm[dst]+i,(gt?(x>y):(x==y))?0xff:0,width);}
}
static void jit_mmx_pack( struct x86cpu *c, int dst, int src, uint32_t a, int ismem, int word, int uns )
{
    uint8_t s[8],d[8],o[8];int i;memcpy(d,c->xmm[dst],8);for(i=0;i<8;i++)s[i]=ismem?rd8(a+(uint32_t)i):c->xmm[src][i];
    if(word)for(i=0;i<2;i++){int32_t x=(int32_t)((uint32_t)d[4*i]|((uint32_t)d[4*i+1]<<8)|((uint32_t)d[4*i+2]<<16)|((uint32_t)d[4*i+3]<<24)),y=(int32_t)((uint32_t)s[4*i]|((uint32_t)s[4*i+1]<<8)|((uint32_t)s[4*i+2]<<16)|((uint32_t)s[4*i+3]<<24));int z=x<-32768?-32768:x>32767?32767:x;uint16_t u=(uint16_t)z;o[2*i]=(uint8_t)u;o[2*i+1]=(uint8_t)(u>>8);z=y<-32768?-32768:y>32767?32767:y;u=(uint16_t)z;o[4+2*i]=(uint8_t)u;o[4+2*i+1]=(uint8_t)(u>>8);}
    else for(i=0;i<4;i++){int16_t x=(int16_t)((uint16_t)d[2*i]|((uint16_t)d[2*i+1]<<8)),y=(int16_t)((uint16_t)s[2*i]|((uint16_t)s[2*i+1]<<8));int z=uns?(x<0?0:x>255?255:x):(x<-128?-128:x>127?127:x);int q=uns?(y<0?0:y>255?255:y):(y<-128?-128:y>127?127:y);o[i]=(uint8_t)z;o[4+i]=(uint8_t)q;}
    memcpy(c->xmm[dst],o,8);
}
static void jit_xmm_packed_addsub( struct x86cpu *c, int dst, int src,
                                   uint32_t a, int ismem, int width, int sub )
{
    uint8_t s[16]; int i;
    for (i=0;i<16;i++) s[i]=ismem ? rd8(a+(uint32_t)i) : c->xmm[src][i];
    if (width==1) for(i=0;i<16;i++) c->xmm[dst][i]=(uint8_t)(sub ? c->xmm[dst][i]-s[i] : c->xmm[dst][i]+s[i]);
    else if (width==2) for(i=0;i<8;i++){ uint16_t x=(uint16_t)c->xmm[dst][i*2]|((uint16_t)c->xmm[dst][i*2+1]<<8), y=(uint16_t)s[i*2]|((uint16_t)s[i*2+1]<<8), z=(uint16_t)(sub?x-y:x+y); c->xmm[dst][i*2]=(uint8_t)z; c->xmm[dst][i*2+1]=(uint8_t)(z>>8); }
    else for(i=0;i<4;i++){ uint32_t x=(uint32_t)c->xmm[dst][i*4]|((uint32_t)c->xmm[dst][i*4+1]<<8)|((uint32_t)c->xmm[dst][i*4+2]<<16)|((uint32_t)c->xmm[dst][i*4+3]<<24), y=(uint32_t)s[i*4]|((uint32_t)s[i*4+1]<<8)|((uint32_t)s[i*4+2]<<16)|((uint32_t)s[i*4+3]<<24), z=sub?x-y:x+y; c->xmm[dst][i*4]=(uint8_t)z; c->xmm[dst][i*4+1]=(uint8_t)(z>>8); c->xmm[dst][i*4+2]=(uint8_t)(z>>16); c->xmm[dst][i*4+3]=(uint8_t)(z>>24); }
}
static void jit_xmm_sat_addsub( struct x86cpu *c, int dst, int src, uint32_t a,
                                int ismem, int width, int sub, int signedv )
{
    uint8_t s[16]; int i; for(i=0;i<16;i++) s[i]=ismem?rd8(a+(uint32_t)i):c->xmm[src][i];
    int n=16/width, max=signedv?(1<<(width*8-1))-1:(1<<(width*8))-1, min=signedv?-(1<<(width*8-1)):0;
    for(i=0;i<n;i++){ uint32_t xo=0,yo=0; memcpy(&xo,c->xmm[dst]+i*width,width); memcpy(&yo,s+i*width,width); int64_t x=signedv?(width==1?(int8_t)xo:(int16_t)xo):(int64_t)xo, y=signedv?(width==1?(int8_t)yo:(int16_t)yo):(int64_t)yo, z=sub?x-y:x+y; if(z<min)z=min; if(z>max)z=max; uint64_t u=(uint64_t)z; memcpy(c->xmm[dst]+i*width,&u,width); }
}
static void jit_xmm_shuffle( struct x86cpu *c, int dst, int src, uint32_t a,
                             int ismem, uint32_t imm, int mode )
{
    uint8_t s[16],d[16],o[16]; int i;
    memcpy(d,c->xmm[dst],16); for(i=0;i<16;i++) s[i]=ismem?rd8(a+(uint32_t)i):c->xmm[src][i];
    if(mode==0) for(i=0;i<4;i++) memcpy(o+i*4,s+((imm>>(2*i))&3)*4,4);
    else { memcpy(o,d,16); if(mode==1) for(i=0;i<4;i++) memcpy(o+i*2,s+((imm>>(2*i))&3)*2,2); else if(mode==2) for(i=0;i<4;i++) memcpy(o+(8+i*2),s+(8+((imm>>(2*i))&3)*2),2); else { for(i=0;i<2;i++) memcpy(o+i*4,d+((imm>>(2*i))&3)*4,4); for(i=0;i<2;i++) memcpy(o+(8+i*4),s+((imm>>(4+2*i))&3)*4,4); } }
    memcpy(c->xmm[dst],o,16);
}
static void jit_xmm_shufps3( struct x86cpu *c, int dst, int src1, int src2,
                             uint32_t a, int ismem, uint32_t imm )
{
    uint8_t x[16],y[16],o[16]; int i; memcpy(x,c->xmm[src1],16);
    if(ismem) for(i=0;i<16;i++) y[i]=rd8(a+(uint32_t)i); else memcpy(y,c->xmm[src2],16);
    for(i=0;i<2;i++) memcpy(o+i*4,x+((imm>>(2*i))&3)*4,4);
    for(i=0;i<2;i++) memcpy(o+(i+2)*4,y+((imm>>(4+2*i))&3)*4,4);
    memcpy(c->xmm[dst],o,16);
}
static void jit_ymm_load( struct x86cpu *c, int dst, uint32_t a )
{ int i; for(i=0;i<32;i++) c->ymm[dst][i]=rd8(a+(uint32_t)i); memcpy(c->xmm[dst],c->ymm[dst],16); }
static void jit_ymm_store( struct x86cpu *c, int src, uint32_t a )
{ int i; for(i=0;i<32;i++) wr8(a+(uint32_t)i,c->ymm[src][i]); }
static void jit_ymm_sync_lower( struct x86cpu *c, int dst )
{ memcpy(c->xmm[dst],c->ymm[dst],16); }
static void jit_ymm_binps3( struct x86cpu *c, int dst, int src1, int src2,
                            uint32_t a, int ismem, int op )
{
    float x,y,r; uint8_t s[32]; int i; if(ismem)for(i=0;i<32;i++)s[i]=rd8(a+(uint32_t)i);
    for(i=0;i<8;i++){memcpy(&x,c->ymm[src1]+i*4,4);memcpy(&y,ismem?s+i*4:c->ymm[src2]+i*4,4);if(op==0)r=x+y;else if(op==1)r=x-y;else if(op==2)r=x*y;else r=x/y;memcpy(c->ymm[dst]+i*4,&r,4);}
    jit_ymm_sync_lower(c,dst);
}
static void jit_ymm_shufps3( struct x86cpu *c, int dst, int src1, int src2,
                             uint32_t a, int ismem, uint32_t imm )
{
    uint8_t x[32],y[32],o[32]; int i; memcpy(x,c->ymm[src1],32); if(ismem)for(i=0;i<32;i++)y[i]=rd8(a+(uint32_t)i);else memcpy(y,c->ymm[src2],32);
    for(i=0;i<2;i++){memcpy(o+i*4,x+((imm>>(2*i))&3)*4,4);memcpy(o+16+i*4,x+16+((imm>>(2*i))&3)*4,4);}
    for(i=0;i<2;i++){memcpy(o+(i+2)*4,y+((imm>>(4+2*i))&3)*4,4);memcpy(o+16+(i+2)*4,y+16+((imm>>(4+2*i))&3)*4,4);}
    memcpy(c->ymm[dst],o,32);
    jit_ymm_sync_lower(c,dst);
}
static void jit_vec_blendps( struct x86cpu *c, int width, int dst, int src1,
                             int src2, uint32_t a, int ismem, uint32_t imm )
{
    uint8_t y[32]; int i,n=width/4; memcpy(width==32?c->ymm[dst]:c->xmm[dst],width==32?c->ymm[src1]:c->xmm[src1],width);
    if(ismem)for(i=0;i<width;i++)y[i]=rd8(a+(uint32_t)i);else memcpy(y,width==32?c->ymm[src2]:c->xmm[src2],width);
    for(i=0;i<n;i++) if(imm&(1u<<i)) memcpy(width==32?c->ymm[dst]+i*4:c->xmm[dst]+i*4,y+i*4,4);
    if(width==32)jit_ymm_sync_lower(c,dst);
}
static void jit_ymm_perm2f128( struct x86cpu *c, int dst, int src1, int src2,
                               uint32_t a, int ismem, uint32_t imm )
{
    uint8_t x[32],b[32],o[32]; int i,sel; memcpy(x,c->ymm[src1],32);
    if(ismem)for(i=0;i<32;i++)b[i]=rd8(a+(uint32_t)i);else memcpy(b,c->ymm[src2],32);
    for(i=0;i<2;i++){
        sel=(imm>>(i*4))&7;
        if(sel&4) memset(o+i*16,0,16);
        else memcpy(o+i*16,(sel&2)?(b+((sel&1)*16)):(x+((sel&1)*16)),16);
    }
    memcpy(c->ymm[dst],o,32); jit_ymm_sync_lower(c,dst);
}
static void jit_xmm_unpcklo_int( struct x86cpu *c, int dst, int src, int width )
{
    uint8_t d[16],s[16],o[16]; int i,n=16/width;
    memcpy(d,c->xmm[dst],16); memcpy(s,c->xmm[src],16);
    for(i=0;i<n/2;i++){ memcpy(o+i*2*width,d+i*width,width); memcpy(o+(i*2+1)*width,s+i*width,width); }
    memcpy(c->xmm[dst],o,16);
}
static void jit_xmm_unpck_int( struct x86cpu *c, int dst, int src, int width, int high )
{
    uint8_t d[16],s[16],o[16]; int i,off=high?8:0, n=8/width;
    memcpy(d,c->xmm[dst],16); memcpy(s,c->xmm[src],16);
    for(i=0;i<n;i++){ memcpy(o+i*2*width,d+off+i*width,width); memcpy(o+(i*2+1)*width,s+off+i*width,width); }
    memcpy(c->xmm[dst],o,16);
}
static void jit_xmm_unpck_mem( struct x86cpu *c, int dst, uint32_t a, int width, int high )
{
    uint8_t d[16],s[16],o[16]; int i,off=high?8:0,n=8/width; memcpy(d,c->xmm[dst],16); for(i=0;i<16;i++)s[i]=rd8(a+(uint32_t)i);
    for(i=0;i<n;i++){memcpy(o+i*2*width,d+off+i*width,width);memcpy(o+(i*2+1)*width,s+off+i*width,width);} memcpy(c->xmm[dst],o,16);
}
static void jit_xmm_pmullw( struct x86cpu *c, int dst, int src, uint32_t a, int ismem )
{
    uint8_t s[16]; int i; for(i=0;i<16;i++) s[i]=ismem?rd8(a+(uint32_t)i):c->xmm[src][i];
    for(i=0;i<8;i++){ int16_t x=(int16_t)((uint16_t)c->xmm[dst][2*i]|((uint16_t)c->xmm[dst][2*i+1]<<8)), y=(int16_t)((uint16_t)s[2*i]|((uint16_t)s[2*i+1]<<8)); uint16_t z=(uint16_t)(x*y); c->xmm[dst][2*i]=(uint8_t)z; c->xmm[dst][2*i+1]=(uint8_t)(z>>8); }
}
static void jit_xmm_pmaddwd( struct x86cpu *c, int dst, int src, uint32_t a, int ismem )
{
    uint8_t s[16],o[16]; int i; for(i=0;i<16;i++) s[i]=ismem?rd8(a+(uint32_t)i):c->xmm[src][i];
    for(i=0;i<4;i++){ int16_t x0=(int16_t)((uint16_t)c->xmm[dst][4*i]|((uint16_t)c->xmm[dst][4*i+1]<<8)), x1=(int16_t)((uint16_t)c->xmm[dst][4*i+2]|((uint16_t)c->xmm[dst][4*i+3]<<8)); int16_t y0=(int16_t)((uint16_t)s[4*i]|((uint16_t)s[4*i+1]<<8)), y1=(int16_t)((uint16_t)s[4*i+2]|((uint16_t)s[4*i+3]<<8)); uint32_t z=(uint32_t)((int32_t)x0*y0+(int32_t)x1*y1); o[4*i]=(uint8_t)z; o[4*i+1]=(uint8_t)(z>>8); o[4*i+2]=(uint8_t)(z>>16); o[4*i+3]=(uint8_t)(z>>24); }
    memcpy(c->xmm[dst],o,16);
}
static void jit_xmm_pack( struct x86cpu *c, int dst, int src, uint32_t a,
                          int ismem, int to_word, int unsignedv )
{
    uint8_t s[16],d[16],o[16]; int i; memcpy(d,c->xmm[dst],16);
    for(i=0;i<16;i++) s[i]=ismem?rd8(a+(uint32_t)i):c->xmm[src][i];
    if (to_word) for(i=0;i<4;i++){ int32_t x=(int32_t)((uint32_t)d[4*i]|((uint32_t)d[4*i+1]<<8)|((uint32_t)d[4*i+2]<<16)|((uint32_t)d[4*i+3]<<24)); int32_t y=(int32_t)((uint32_t)s[4*i]|((uint32_t)s[4*i+1]<<8)|((uint32_t)s[4*i+2]<<16)|((uint32_t)s[4*i+3]<<24)); int32_t z=x<-32768?-32768:x>32767?32767:x; uint16_t u=(uint16_t)z; o[2*i]=(uint8_t)u;o[2*i+1]=(uint8_t)(u>>8); z=y<-32768?-32768:y>32767?32767:y;u=(uint16_t)z;o[8+2*i]=(uint8_t)u;o[8+2*i+1]=(uint8_t)(u>>8); }
    else for(i=0;i<8;i++){ int32_t x=(int16_t)((uint16_t)d[2*i]|((uint16_t)d[2*i+1]<<8)), y=(int16_t)((uint16_t)s[2*i]|((uint16_t)s[2*i+1]<<8)); int32_t z=unsignedv?(x<0?0:x>255?255:x):(x<-128?-128:x>127?127:x); int32_t q=unsignedv?(y<0?0:y>255?255:y):(y<-128?-128:y>127?127:y); o[i]=(uint8_t)z;o[8+i]=(uint8_t)q; }
    memcpy(c->xmm[dst],o,16);
}
static void jit_xmm_avg( struct x86cpu *c, int dst, int src, uint32_t a, int ismem, int width )
{
    uint8_t s[16]; int i; for(i=0;i<16;i++)s[i]=ismem?rd8(a+(uint32_t)i):c->xmm[src][i];
    if(width==1)for(i=0;i<16;i++)c->xmm[dst][i]=(uint8_t)(((unsigned)c->xmm[dst][i]+s[i]+1)>>1);
    else for(i=0;i<8;i++){uint16_t x=(uint16_t)c->xmm[dst][2*i]|((uint16_t)c->xmm[dst][2*i+1]<<8),y=(uint16_t)s[2*i]|((uint16_t)s[2*i+1]<<8),z=(uint16_t)((x+y+1)>>1);c->xmm[dst][2*i]=(uint8_t)z;c->xmm[dst][2*i+1]=(uint8_t)(z>>8);}
}
static void jit_xmm_cmpgt( struct x86cpu *c, int dst, int src, uint32_t a, int ismem, int width )
{
    uint8_t s[16],d[16],o[16]; int i; memcpy(d,c->xmm[dst],16);for(i=0;i<16;i++)s[i]=ismem?rd8(a+(uint32_t)i):c->xmm[src][i];
    for(i=0;i<16;i+=width){int64_t x=width==1?(int8_t)d[i]:(int16_t)((uint16_t)d[i]|((uint16_t)d[i+1]<<8));int64_t y=width==1?(int8_t)s[i]:(int16_t)((uint16_t)s[i]|((uint16_t)s[i+1]<<8));if(width==4){x=(int32_t)((uint32_t)d[i]|((uint32_t)d[i+1]<<8)|((uint32_t)d[i+2]<<16)|((uint32_t)d[i+3]<<24));y=(int32_t)((uint32_t)s[i]|((uint32_t)s[i+1]<<8)|((uint32_t)s[i+2]<<16)|((uint32_t)s[i+3]<<24));}memset(o+i,x>y?0xff:0,width);}
    memcpy(c->xmm[dst],o,16);
}
static void jit_xmm_cmpeq( struct x86cpu *c, int dst, int src, uint32_t a, int ismem, int width )
{
    uint8_t s[16],d[16],o[16];int i;memcpy(d,c->xmm[dst],16);for(i=0;i<16;i++)s[i]=ismem?rd8(a+(uint32_t)i):c->xmm[src][i];
    for(i=0;i<16;i+=width){int eq=1;for(int k=0;k<width;k++)if(d[i+k]!=s[i+k])eq=0;memset(o+i,eq?0xff:0,width);}memcpy(c->xmm[dst],o,16);
}
static void jit_xmm_minmaxub( struct x86cpu *c, int dst, int src, uint32_t a, int ismem, int ismax )
{
    int i; for(i=0;i<16;i++){uint8_t s=ismem?rd8(a+(uint32_t)i):c->xmm[src][i];if(ismax){if(s>c->xmm[dst][i])c->xmm[dst][i]=s;}else if(s<c->xmm[dst][i])c->xmm[dst][i]=s;}
}
static void jit_xmm_pmaddubsw( struct x86cpu *c, int dst, int src, uint32_t a, int ismem )
{
    uint8_t s[16],o[16];int i;for(i=0;i<16;i++)s[i]=ismem?rd8(a+(uint32_t)i):c->xmm[src][i];
    for(i=0;i<8;i++){int z=(int)c->xmm[dst][2*i]*(int8_t)s[2*i]+(int)c->xmm[dst][2*i+1]*(int8_t)s[2*i+1];if(z>32767)z=32767;if(z<-32768)z=-32768;o[2*i]=(uint8_t)z;o[2*i+1]=(uint8_t)(z>>8);}memcpy(c->xmm[dst],o,16);
}
static void jit_xmm_shift_bytes( struct x86cpu *c, int dst, uint32_t count, int left )
{
    uint8_t o[16]; int i,n=count>16?16:(int)count; memset(o,0,16); if(n<16)for(i=0;i<16-n;i++)o[left?i+n:i]=c->xmm[dst][left?i: i+n]; memcpy(c->xmm[dst],o,16);
}
static void jit_xmm_shift( struct x86cpu *c, int dst, uint32_t count, int width, int arith, int left )
{
    int i,n=16/width,bits=width*8; if(count>= (uint32_t)bits){ for(i=0;i<n;i++){ int neg=arith&&!left&&(c->xmm[dst][i*width+width-1]&0x80); memset(c->xmm[dst]+i*width,neg?0xff:0,width); } return; }
    for(i=0;i<n;i++){ uint64_t v=0; memcpy(&v,c->xmm[dst]+i*width,width); if(left) v<<=count; else if(arith){ int64_t s=(int64_t)(v<<(64-bits)); s>>=(64-bits); v=(uint64_t)(s>>count); } else v>>=count; memcpy(c->xmm[dst]+i*width,&v,width); }
}
static uint32_t jit_xmm_count( struct x86cpu *c, int src )
{
    uint32_t v; memcpy(&v,c->xmm[src],4); return v;
}
static void jit_rep_stos( struct x86cpu *c, int size )
{
    uint32_t p=c->regs[EDI],n=c->regs[ECX],v=c->regs[EAX]; int32_t step=(c->eflags&DF)?-size:size;
    while(n--){if(size==1)wr8(p,(uint8_t)v);else if(size==2)wr16(p,(uint16_t)v);else wr32(p,v);p+=(uint32_t)step;} c->regs[EDI]=p;c->regs[ECX]=0;
}
static void jit_rep_movs( struct x86cpu *c, int size )
{
    uint32_t s=c->regs[ESI],d=c->regs[EDI],n=c->regs[ECX]; int32_t step=(c->eflags&DF)?-size:size;
    while(n--){uint32_t v=size==1?rd8(s):size==2?rd16(s):rd32(s);if(size==1)wr8(d,(uint8_t)v);else if(size==2)wr16(d,(uint16_t)v);else wr32(d,v);s+=(uint32_t)step;d+=(uint32_t)step;} c->regs[ESI]=s;c->regs[EDI]=d;c->regs[ECX]=0;
}
static void jit_string_one( struct x86cpu *c, int size, int store )
{
    uint32_t s=c->regs[ESI],d=c->regs[EDI],v=c->regs[EAX]; int32_t step=(c->eflags&DF)?-size:size;
    if(store){if(size==1)wr8(d,(uint8_t)v);else if(size==2)wr16(d,(uint16_t)v);else wr32(d,v);}
    else {v=size==1?rd8(s):size==2?rd16(s):rd32(s);if(size==1)c->regs[EAX]=(c->regs[EAX]&0xffffff00u)|v;else if(size==2)c->regs[EAX]=(c->regs[EAX]&0xffff0000u)|v;else c->regs[EAX]=v;s+=(uint32_t)step;}
    if(store)c->regs[EDI]+=step;else {c->regs[ESI]=s;c->regs[EDI]+=step;}
}
static void jit_cmpxchg8b( struct x86cpu *c, uint32_t a )
{
    uint64_t mem=(uint64_t)rd32(a)|((uint64_t)rd32(a+4)<<32),cmp=(uint64_t)c->regs[EAX]|((uint64_t)c->regs[EDX]<<32);
    c->eflags=get_flags(c); if(mem==cmp){wr32(a,c->regs[EBX]);wr32(a+4,c->regs[ECX]);c->eflags|=ZF;}
    else{c->regs[EAX]=(uint32_t)mem;c->regs[EDX]=(uint32_t)(mem>>32);c->eflags&=~ZF;} c->lf_size=0;
}
static void jit_x87_bin( struct x86cpu *c, int op, int dst, double src,
                         int pop, int reverse )
{
    double a=fp_get(c,dst), r;
    if (op==0) r=a+src; else if (op==1) r=a*src;
    else if (op==2) r=reverse ? src-a : a-src;
    else r=reverse ? src/a : a/src;
    fp_set(c,dst,r); if (pop) fp_pop(c);
}
/* Single-step exactly one guest instruction through the interpreter, for an
 * opcode the translator does not implement (x87/SSE and other rarities).
 *
 * This is what keeps a block WHOLE: an unhandled instruction used to end the
 * block (or kill it outright), so the surrounding integer code fell back to the
 * interpreter and the chain broke.  Now the block calls in for that one
 * instruction and carries straight on.
 *
 * Re-entrancy: g_jit_filling suppresses JIT dispatch inside the nested run(),
 * and g_verify_budget=1 makes it retire exactly one instruction; both are
 * saved/restored so the verify path can use this too.  Writes still go through
 * wr8/16/32, so the verify undo log stays complete.
 *
 * Returns 0 if the instruction did NOT simply fall through to `next` (it
 * branched, trapped, or stopped the CPU) - the caller then leaves the block and
 * lets the main loop resume from the real eip.  That check is what makes this
 * safe for ANY opcode, including ones that transfer control. */
static int jit_step1( struct x86cpu *c, uint32_t at, uint32_t next )
{
    int sf = g_jit_filling, sb = g_verify_budget;
    g_jit_filling = 1; g_verify_budget = 1;
    c->eip = at;
    c->running = 1;
    run( c );
    g_jit_filling = sf; g_verify_budget = sb;
    /* run() counted this instruction too; the block's ninsn already includes
     * it, so drop the duplicate to keep the mips figure honest. */
    g_total_insns--;
    return c->eip == next && c->running;
}

#include "gen_blocks.c"
#ifdef WEBWINE_GENBLOCKS
#include "gdi32_gen_blocks.c"
#endif
#if defined(WEBWINE_GENBLOCKS) && defined(WEBWINE_FP_HOT)
#include "fp_hot_gen_blocks.c"
#endif
#if defined(WEBWINE_GENBLOCKS) && defined(WEBWINE_MSVCRT_AOT)
#ifdef WEBWINE_MSVCRT_FOCUSED_AOT
#include "msvcrt_focused_gen_blocks.c"
#else
#include "msvcrt_gen_blocks.c"
#endif
#endif
#ifdef WEBWINE_GENBLOCKS
#define GDI32_GEN_HASHN (1u << 15)
static uint32_t gdi32_gen_va[GDI32_GEN_HASHN];
static uint32_t gdi32_gen_end[GDI32_GEN_HASHN];
static uint16_t gdi32_gen_ninsn[GDI32_GEN_HASHN];
static int (*gdi32_gen_fn[GDI32_GEN_HASHN])(struct x86cpu *);
static int32_t gdi32_gen_idx[GDI32_GEN_HASHN];
static uint32_t gdi32_gen_nblk;
static void gdi32_gen_build_hash( void )
{
    for (int i = 0; gdi32_g_genblk[i].fn; i++)
    {
        uint32_t va = gdi32_g_genblk[i].va;
        unsigned h = (unsigned)((va * 2654435761u) >> (32 - 10));
        while (gdi32_gen_va[h]) h = (h + 1) & (GDI32_GEN_HASHN - 1);
        gdi32_gen_va[h] = va; gdi32_gen_end[h] = gdi32_g_genblk[i].end;
        gdi32_gen_ninsn[h] = gdi32_g_genblk[i].ninsn;
        gdi32_gen_fn[h] = gdi32_g_genblk[i].fn; gdi32_gen_idx[h] = i;
        gdi32_gen_nblk++;
    }
    fprintf( stderr, "wasm_x86: gdi32 JIT %u translated blocks loaded\\n", gdi32_gen_nblk );
}
static int gdi32_gen_lookup( uint32_t va )
{
    unsigned h = (unsigned)((va * 2654435761u) >> (32 - 10));
    while (gdi32_gen_va[h])
    { if (gdi32_gen_va[h] == va) return (int)h; h = (h + 1) & (GDI32_GEN_HASHN - 1); }
    return -1;
}
#endif
#define GEN_HASHN (1u << 19)
static uint32_t g_gen_va[GEN_HASHN];
static uint32_t g_gen_end[GEN_HASHN];
static uint16_t g_gen_ninsn[GEN_HASHN];
static int    (*g_gen_fn[GEN_HASHN])( struct x86cpu * );
static int32_t  g_gen_idx[GEN_HASHN];   /* g_genblk[] index for this hash slot */
static uint64_t g_jit_insns, g_jit_blocks, g_jit_last_insns, g_jit_last_blocks;
static uint32_t g_gen_nblk;
#if defined(WEBWINE_GENBLOCKS) && defined(WEBWINE_MSVCRT_AOT)
#define MSVCRT_GEN_HASHN (1u << 16)
static uint32_t msvcrt_gen_va[MSVCRT_GEN_HASHN];
static uint32_t msvcrt_gen_end[MSVCRT_GEN_HASHN];
static uint16_t msvcrt_gen_ninsn[MSVCRT_GEN_HASHN];
static int (*msvcrt_gen_fn[MSVCRT_GEN_HASHN])(struct x86cpu *);
static int32_t msvcrt_gen_idx[MSVCRT_GEN_HASHN];
static uint32_t msvcrt_gen_nblk;
static void msvcrt_gen_build_hash( void )
{
    for (int i = 0; msvcrt_g_genblk[i].fn; i++)
    {
        uint32_t va = msvcrt_g_genblk[i].va;
        unsigned h = (unsigned)((va * 2654435761u) >> (32 - 16));
        while (msvcrt_gen_va[h]) h = (h + 1) & (MSVCRT_GEN_HASHN - 1);
        msvcrt_gen_va[h] = va; msvcrt_gen_end[h] = msvcrt_g_genblk[i].end;
        msvcrt_gen_ninsn[h] = msvcrt_g_genblk[i].ninsn;
        msvcrt_gen_fn[h] = msvcrt_g_genblk[i].fn; msvcrt_gen_idx[h] = i;
        msvcrt_gen_nblk++;
    }
    fprintf( stderr, "wasm_x86: msvcrt JIT %u translated blocks loaded\n", msvcrt_gen_nblk );
}
static int msvcrt_gen_lookup( uint32_t va )
{
    unsigned h = (unsigned)((va * 2654435761u) >> (32 - 16));
    while (msvcrt_gen_va[h])
    { if (msvcrt_gen_va[h] == va) return (int)h; h = (h + 1) & (MSVCRT_GEN_HASHN - 1); }
    return -1;
}
#endif
#if defined(WEBWINE_GENBLOCKS) && defined(WEBWINE_FP_HOT)
#define FPHOT_GEN_HASHN (1u << 15)
static uint32_t fp_hot_gen_va[FPHOT_GEN_HASHN];
static uint32_t fp_hot_gen_end[FPHOT_GEN_HASHN];
static uint16_t fp_hot_gen_ninsn[FPHOT_GEN_HASHN];
static int (*fp_hot_gen_fn[FPHOT_GEN_HASHN])(struct x86cpu *);
static int32_t fp_hot_gen_idx[FPHOT_GEN_HASHN];
static uint32_t fp_hot_gen_nblk;
static int fp_hot_jit, fp_hot_verify;
static void fp_hot_gen_build_hash( void )
{
    for (int i = 0; fp_g_genblk[i].fn; i++)
    {
        uint32_t va = fp_g_genblk[i].va;
        unsigned h = (unsigned)((va * 2654435761u) >> (32 - 15));
        while (fp_hot_gen_va[h]) h = (h + 1) & (FPHOT_GEN_HASHN - 1);
        fp_hot_gen_va[h] = va; fp_hot_gen_end[h] = fp_g_genblk[i].end;
        fp_hot_gen_ninsn[h] = fp_g_genblk[i].ninsn;
        fp_hot_gen_fn[h] = fp_g_genblk[i].fn; fp_hot_gen_idx[h] = i;
        fp_hot_gen_nblk++;
    }
    fprintf( stderr, "wasm_x86: floating-point hot JIT %u translated blocks loaded\n", fp_hot_gen_nblk );
}
static int fp_hot_gen_lookup( uint32_t va )
{
    unsigned h = (unsigned)((va * 2654435761u) >> (32 - 15));
    while (fp_hot_gen_va[h])
    { if (fp_hot_gen_va[h] == va) return (int)h; h = (h + 1) & (FPHOT_GEN_HASHN - 1); }
    return -1;
}
static int fp_hot_verify_block( struct x86cpu *c, int slot, uint32_t start )
{
    struct x86cpu saved = *c, jit;
    int sf = g_jit_filling, sb = g_verify_budget, bad = 0;
    g_wlog_n = 0; g_wlog_of = 0; g_jit_recording = 1;
    fp_hot_gen_fn[slot]( c );
    g_jit_recording = 0; jit = *c;
    if (!g_wlog_of)
    {
        for (int i = g_wlog_n - 1; i >= 0; i--)
        { uint32_t a = g_wlog[i].addr;
          if (g_wlog[i].size == 1) *(uint8_t *)(uintptr_t)a = (uint8_t)g_wlog[i].old;
          else if (g_wlog[i].size == 2) *(uint16_t *)(uintptr_t)a = (uint16_t)g_wlog[i].old;
          else *(uint32_t *)(uintptr_t)a = g_wlog[i].old; }
        *c = saved; g_verify_budget = fp_hot_gen_ninsn[slot]; g_jit_filling = 1;
        c->running = 1; run( c ); g_jit_filling = sf; g_verify_budget = sb;
        for (int r = 0; r < 8; r++) if (c->regs[r] != jit.regs[r]) bad = 1;
        if (!bad && c->eflags != jit.eflags) bad = 1;
        if (!bad && c->eip != jit.eip) bad = 1;
        if (!bad && c->lf_kind != jit.lf_kind) bad = 1;
        if (!bad && c->lf_size != jit.lf_size) bad = 1;
        if (!bad && c->lf_op1 != jit.lf_op1) bad = 1;
        if (!bad && c->lf_op2 != jit.lf_op2) bad = 1;
        if (!bad && c->lf_res != jit.lf_res) bad = 1;
        if (!bad && c->lf_cin != jit.lf_cin) bad = 1;
        if (!bad && c->fptop != jit.fptop) bad = 1;
        if (!bad && c->fpcw != jit.fpcw) bad = 1;
        if (!bad && c->fpsw != jit.fpsw) bad = 1;
        if (!bad && memcmp( c->fpr, jit.fpr, sizeof(c->fpr) )) bad = 1;
        if (!bad) for (int r = 0; r < 8; r++) for (int k = 0; k < 16; k++)
            if (c->xmm[r][k] != jit.xmm[r][k]) { bad = 1; break; }
        if (!bad) for (int r = 0; r < 8; r++) for (int k = 0; k < 32; k++)
            if (c->ymm[r][k] != jit.ymm[r][k]) { bad = 1; break; }
        if (bad)
            fprintf( stderr, "JITBAD FP blk=%08x jit_eip=%08x int_eip=%08x\n",
                     start - (uint32_t)nd_slide, jit.eip - (uint32_t)nd_slide,
                     c->eip - (uint32_t)nd_slide );
        else *c = jit;
    }
    else { *c = jit; g_jit_filling = sf; g_verify_budget = sb; }
    return !bad;
}
#endif
static void gen_build_hash( void )
{
    for (int i = 0; g_genblk[i].fn; i++)
    {
        uint32_t va = g_genblk[i].va;
        unsigned h = (unsigned)((va * 2654435761u) >> (32 - 19));
        while (g_gen_va[h]) h = (h + 1) & (GEN_HASHN - 1);
        g_gen_va[h] = va; g_gen_end[h] = g_genblk[i].end;
        g_gen_ninsn[h] = g_genblk[i].ninsn; g_gen_fn[h] = g_genblk[i].fn;
        g_gen_idx[h] = i; g_gen_nblk++;
    }
    fprintf( stderr, "wasm_x86: JIT %u translated blocks loaded\n", g_gen_nblk );
}
static int gen_lookup( uint32_t va )   /* slot index, or -1 */
{
    unsigned h = (unsigned)((va * 2654435761u) >> (32 - 19));
    while (g_gen_va[h]) { if (g_gen_va[h] == va) return (int)h; h = (h + 1) & (GEN_HASHN - 1); }
    return -1;
}
#define GEN_LOOKUP_CACHE_N 1024
/* Dynamic-code misses tend to revisit a small loop body, but not necessarily
 * the same EIP twice in a row.  Keep the negative results too: unlike the
 * generated-block table this cache is only a dispatch hint and can be
 * invalidated without touching translated code. */
static uint32_t g_gen_cache_va[GEN_LOOKUP_CACHE_N];
static int32_t g_gen_cache_slot[GEN_LOOKUP_CACHE_N];
static int g_gen_cache_on = -1;
static int gen_lookup_cached( uint32_t va )
{
    unsigned i = (unsigned)((va * 2654435761u) >> (32 - 10));
    uint32_t key = va + 1u;
    int slot;
    if (g_gen_cache_on == -1) g_gen_cache_on = getenv( "WASM_NO_GENCACHE" ) ? 0 : 1;
    if (!g_gen_cache_on) return gen_lookup( va );
    if (g_gen_cache_va[i] == key) return g_gen_cache_slot[i];
    slot = gen_lookup( va );
    g_gen_cache_va[i] = key;
    g_gen_cache_slot[i] = slot;
    return slot;
}
#endif

#define IPAGE_COUNT 65536
static uint32_t g_ipage[IPAGE_COUNT]; static int g_ipage_on; /* full 64KB guest-page tally */
static uint32_t g_ipage_eip[IPAGE_COUNT];                 /* representative eip per page */
static int ipage_count_cmp( const void *a, const void *b )
{
    unsigned x = *(const unsigned *)a, y = *(const unsigned *)b;
    if (g_ipage[x] < g_ipage[y]) return 1;
    if (g_ipage[x] > g_ipage[y]) return -1;
    return (x > y) - (x < y);
}
#define IMISS_COUNT (1u << 18)
static uint32_t g_imiss_va[IMISS_COUNT];
static uint32_t g_imiss_n[IMISS_COUNT];
static int g_ipage_frame_mode, g_ipage_frame_started;
static int g_msvcrt_miss_trace = -1;
static unsigned g_msvcrt_miss_trace_n;
static int g_msvcrt_hot_trace = -1;
static unsigned g_msvcrt_hot_trace_n;
static int g_vline1_entry_trace = -1;
static unsigned g_vline1_entry_trace_n;
static void run( struct x86cpu *c )
{
    uint32_t last_eip = c->eip;
    /* Instruction count lives in a local and is folded into the global only at
     * the housekeeping tick and on the way out (see the RUN_RETURN sites).
     * Incrementing the 64-bit global directly cost an i64 load + i64 store to
     * linear memory on EVERY guest instruction, purely for diagnostics.  A local
     * is a wasm register, and accumulating a delta (rather than mirroring the
     * total) stays correct when run() re-enters itself for user callbacks. */
    uint32_t idelta = 0;
    c->running = 1;
    while (c->running)
    {
        /* Periodic housekeeping (~every 64K insns): derive the ASLR slide once
         * the PEB is ready (which arms frame-boundary presenting), and — under
         * WASM_TPUT — sample throughput/fps.  Kept off the per-instruction path
         * so the hot loop stays tight. */
#ifdef WASM_X86_PROFILE
        /* Sampling profiler (diagnostic builds only).  The stride is JITTERED:
         * sampling on the fixed 64K housekeeping tick aliases against tight
         * loops and once reported a 5-instruction engine loop at ~30% of the
         * frame when a direct counter proved it was ~6%. */
        if (g_prof_on && --g_prof_countdown <= 0)
        {
            static uint32_t lcg = 12345;
            lcg = lcg * 1664525u + 1013904223u;
            /* Take the HIGH bits.  An LCG's low bits have tiny periods (bit 0
             * alternates, bit 1 has period 4...), so `lcg % 50000` produced a
             * strongly periodic "jitter" that still aliased against per-frame
             * work: it read the audio mixer at 6.2% of the frame where two
             * independent direct counters both put it at 1.8%. */
            g_prof_countdown = 40000 + (int)(((uint64_t)(lcg >> 16) * 50000u) >> 16);
            prof_sample( c->eip );
        }
#endif
        if (++idelta == 0x10000)
        {
            g_total_insns += idelta;
            idelta = 0;
            extern double emscripten_get_now( void );
            if (!g_slide_ok)
            {
                uint32_t teb = c->fs_base;
                uint32_t peb = teb ? rd32( teb + 0x30 ) : 0;
                uint32_t ib  = peb ? rd32( peb + 0x08 ) : 0;
                if (ib) { nd_slide = (int32_t)(ib - 0x400000); g_slide_ok = 1;
#ifdef WEBWINE_GENBLOCKS
                          /* The browser's guest executes self-modifying CON
                           * code whose arena can relocate while a level is
                           * loading.  A static AOT chain is unsafe across
                           * that relocation, so make browser JIT opt-in until
                           * the dynamic-code invalidation boundary is fixed.
                           * Native verified hooks still provide the stable
                           * renderer fast paths. */
#ifdef WEBWINE_BROWSER
                          g_jit_on = getenv( "WASM_JIT" ) && !getenv( "WASM_NO_JIT" );
#else
                          g_jit_on = getenv( "WASM_NO_JIT" ) ? 0 : 1;
#endif
                          g_jit_verify = getenv( "WASM_JIT_VERIFY" ) ? 1 : 0;
                          if (g_jit_on) gen_build_hash();
#if defined(WEBWINE_FP_HOT)
                          fp_hot_jit = getenv( "WASM_FP_HOT" ) ? 1 : 0;
                          fp_hot_verify = getenv( "WASM_FP_HOT_VERIFY" ) ? 1 : 0;
                          if (fp_hot_jit) fp_hot_gen_build_hash();
#endif
#endif
                          g_flip_addr = ND_VIDEONEXTPAGE + (uint32_t)nd_slide;
                          nat_register( g_flip_addr, NAT_FLIP, "frame flip" );
                          nat_arm_pow();
                          nat_arm_pthread_spin_unlock();
                          nat_arm_pthread_spin_lock();
                          nat_arm_pthread_getspecific();
                          {   /* WASM_COUNT_ADDR=<hex> counts executions of any guest
                               * address - the sampler over-attributes tight loops, so
                               * confirm hot-loop claims with this before acting. */
                              const char *ca = getenv( "WASM_COUNT_ADDR" );
                              if (ca && *ca)
                                  nat_register( (uint32_t)strtoul( ca, NULL, 0 ) + (uint32_t)nd_slide,
                                                NAT_COUNT, "count probe" );
                              else if (getenv( "WASM_COUNT_LOOP" ))
                                  nat_register( ND_AGELOOP + (uint32_t)nd_slide, NAT_COUNT, "loop probe" );
                          }
                          if (!getenv( "WASM_NO_AGELOOP" ) && !getenv( "WASM_COUNT_LOOP" ))
                              nat_arm_ageloop();
#ifdef WEBWINE_BROWSER
                          /* ageBlocks mutates the live texture-cache allocator.
                           * Keep the guest implementation in browser builds;
                           * the native shortcut is available explicitly for
                           * controlled throughput experiments. */
                          if (getenv( "WASM_AGEBLOCKS" ) && !getenv( "WASM_NO_AGEBLOCKS" ))
                              nat_arm_ageblocks();
#else
                          if (!getenv( "WASM_NO_AGEBLOCKS" )) nat_arm_ageblocks();
#endif
                          /* The setup prologue and body are now both native;
                           * the setup hook keeps every SMC-patched immediate
                           * synchronized before qrhline reads it. */
                          /* qrhline's interpreter fallback can occupy the
                           * render thread for seconds in the attract loop.
                           * The setup hook synchronizes its SMC immediates;
                           * keep this verified fast path enabled by default. */
                          if (browser_fast_render() && !getenv( "WASM_NO_QRHLINE" )) {
                              nat_arm_setupqrhline();
                              nat_arm_qrhline();
                          }
                          if (!getenv( "WASM_NO_AUDIO" )) nat_arm_audio();
                          if (browser_fast_columns() && !getenv( "WASM_NO_VLINE" )) nat_arm_vline();
                          if (browser_fast_columns() && !getenv( "WASM_NO_VLINE_DISPATCH" )) nat_arm_vline_dispatch();
                          if (browser_fast_masked_columns() && !getenv( "WASM_NO_MVLINE" )) nat_arm_mvline();
                          if (browser_fast_masked_columns() && !getenv( "WASM_NO_MVLINE_DISPATCH" )) nat_arm_mvline_dispatch();
                          if (browser_fast_render() && !getenv( "WASM_NO_MHLINE" )) nat_arm_mhline();
                          if (browser_fast_render() && !getenv( "WASM_NO_VLINE1" )) nat_arm_vline1();
                          if (browser_fast_render() && !getenv( "WASM_NO_MVLINE1" )) nat_arm_mvline1();
                          nat_arm_setup_mappers();
                          if (!getenv( "WASM_NO_SDL_TLSGET" )) nat_arm_sdl_tlsget();
                          if (!getenv( "WASM_NO_SDL_ATOMIC_GET" )) nat_arm_sdl_atomics();
                          if (getenv( "WASM_EXPERIMENT_NP2" )) nat_arm_line1np2();
                          if (!getenv( "WASM_NO_CRC32" )) nat_arm_crc32();
                          if (!getenv( "WASM_NO_MIXSTEREO" )) nat_arm_mixstereo();
                          if (!getenv( "WASM_NO_MIDEBUG" )) nat_arm_midebug();
                          if (!getenv( "WASM_NO_PALMATCH" )) nat_arm_pal_closest();
                          if (!getenv( "WASM_NO_GLSTUB" )) { nat_arm_inthash(); nat_arm_glsampler(); }
                          if (browser_fast_render() && !getenv( "WASM_NO_SURFBLIT" )) nat_arm_surfblit();
                          g_skip_blit = getenv( "WASM_KEEP_BLIT" ) ? 0 : 1;
                          /* The span mapper touches the engine's self-growing
                           * cache while a level is being assembled.  Keep the
                           * interpreter implementation as the browser-safe
                           * default until that SMC boundary is independently
                           * verified; opt into the native hook for profiling. */
                          if (getenv( "WASM_SURFSPAN" ) && !getenv( "WASM_NO_SURFSPAN" ))
                              nat_arm_surfspan();
                          g_ld_verify = getenv( "WASM_LIBDIV_VERIFY" ) ? 1 : 0;
                          if (browser_fast_libdiv() && !getenv( "WASM_NO_LIBDIV" )) nat_arm_libdivide();
                          if (!getenv( "WASM_NO_MOUSE" )) nat_arm_mouse();
                          fprintf( stderr, "wasm_x86: exe base=%08x slide=%d\n", ib, nd_slide ); }
            }
            { static int dumped;
              const char *da = getenv( "WASM_DUMP_ADDR" );
              if (!dumped && da && *da)
              {
                  uint32_t a = (uint32_t)strtoul( da, NULL, 0 ) + (uint32_t)nd_slide;
                  int nonzero = 0;
                  for (unsigned di = 0; di < 256; di++) if (rd8( a + di )) { nonzero = 1; break; }
                  /* Runtime-generated code may not exist at the first tick. */
                  if (nonzero)
                  {
                      dumped = 1;
                      fprintf( stderr, "wasm_x86: bytes @%08x:", a - (uint32_t)nd_slide );
                      for (unsigned di = 0; di < 256; di++) fprintf( stderr, " %02x", rd8( a + di ) );
                      fprintf( stderr, "\n" );
                  }
              }
            }
            /* Look for msvcrt every ~256 ticks until found: walking the loader
             * list on every tick measurably slowed boot. */
            /* Audio starvation escape hatch.
             *
             * Audio is normally topped up at the frame flip, which is a clean
             * guest function entry (see the flip handler).  But the game can
             * BLOCK waiting for the mixer to drain before it ever reaches the
             * next flip - SDL's mixer waits on its own mutex - and with a single
             * guest thread that is a deadlock: no flip, so no audio; no audio, so
             * no progress; and it spins in SDL_LockMutex_srw forever.  That is
             * exactly what hung "new game" after the skill screen.
             *
             * So if flips have stopped for far longer than a frame, pump from
             * here regardless.  This runs only when the game is already stuck, so
             * it does not put the mixer back on the arbitrary-boundary path in
             * normal play. */
            {
                static uint64_t last_flip;
                static int stall;
                if (g_flip_count != last_flip) { last_flip = g_flip_count; stall = 0; }
                else if (++stall > 60)          /* ~4M instructions without a frame */
                {
                    stall = 0;
                    audio_pump( c );
                }

                /* WASM_STALLBT=1: when the render loop has gone a very long time
                 * without a flip (a load-time spin, not a normal frame), dump the
                 * guest EIP and walk the EBP chain once per stall episode so we
                 * can see which guest function is looping. */
                {
                    static int on = -1, longstall = 0, ndump = 0;
                    if (on == -1) on = getenv( "WASM_STALLBT" ) ? 1 : 0;
                    if (on)
                    {
                        if (g_flip_count != last_flip) { longstall = 0; ndump = 0; }
                        else if (++longstall > 3000 && (longstall % 4000) == 3001 && ndump < 20)
                        {
                            ndump++;
                            uint32_t bp = c->regs[EBP], eip = c->eip;
                            int d;
                            fprintf( stderr, "wasm_x86: STALLBT flips=%llu eip=%08x", (unsigned long long)g_flip_count, eip );
                            for (d = 0; d < 16; d++)
                            {
                                uint32_t ret, next;
                                if (bp < 0x10000 || bp >= 0x70000000) break;
                                ret  = rd32( bp + 4 );
                                next = rd32( bp );
                                fprintf( stderr, " <%08x", ret );
                                if (next <= bp) break;
                                bp = next;
                            }
                            fprintf( stderr, "\n" );
                        }
                    }
                }
            }
            { static uint32_t nat_tries;
              /* msvcrt and the graphics/user DLLs do not necessarily arrive
               * in the loader list together.  Keep probing for the latter
               * after the CRT hooks have armed; otherwise a late-loaded
               * relocatable relay cluster is silently missed forever. */
              int need_dll_hooks = !g_nat_ready;
#ifdef WEBWINE_GENBLOCKS
              need_dll_hooks = need_dll_hooks || !gdi32_gen_loaded;
#endif
              if (need_dll_hooks && g_slide_ok && (nat_tries++ & 0xff) == 0 && nat_tries < 400000) nat_init( c ); }
            { static int md = -1;
              if (md == -1) md = getenv( "WASM_MODULES" ) ? 0 : 1;
              /* The first housekeeping tick happens while only the exe is in
               * the loader list.  Wait for the normal DLL closure so the
               * diagnostic can map interpreted pages to their real owner. */
              if (!md && g_slide_ok && dump_modules( c ) >= 5) md = 1; }
            { static int gl_on = -1;
              if (gl_on == -1) gl_on = getenv( "WASM_NO_GLTHUNK" ) ? 0 : 1;
              if (gl_on && g_slide_ok && !g_gl_armed) nat_arm_glthunks( c );
              if (gl_on && g_slide_ok && !g_glext_armed) nat_arm_glext( c ); }
            /* Cache the live frameplace pointer while it is valid, so the flip
             * handler can present the finished frame after it is cleared to 0. */
            if (g_slide_ok)
            {
                uint32_t fp = rd32( ND_FRAMEPLACE + nd_slide );
                if (fp) { g_last_fp = fp; g_last_bpl = rd32( ND_BYTESPERLINE + nd_slide );
                          g_last_w = (int)rd32( ND_XDIM + nd_slide ); g_last_h = (int)rd32( ND_YDIM + nd_slide ); }
            }
            static int tp_on = -1;
            static double tp_start = 0, tp_last = 0;
            static uint64_t tp_last_insns = 0, tp_last_flip = 0, tp_last_hits = 0, tp_last_audio = 0, tp_last_calls = 0, tp_last_vlc = 0, tp_last_vli = 0, tp_last_sbc = 0, tp_last_sbi = 0, tp_last_mvc = 0, tp_last_mvi = 0, tp_last_mhc = 0, tp_last_mhi = 0, tp_last_v1c = 0, tp_last_v1i = 0, tp_last_mv1c = 0, tp_last_mv1i = 0, tp_last_v1nc = 0, tp_last_v1ni = 0, tp_last_mv1nc = 0, tp_last_mv1ni = 0;
            if (tp_on == -1) { tp_on = getenv( "WASM_TPUT" ) ? 1 : 0; g_histo_on = getenv( "WASM_HISTO" ) ? 1 : 0;
                               g_prof_on = getenv( "WASM_PROF" ) ? 1 : 0; g_ipage_on = getenv( "WASM_IPAGE" ) ? 1 : 0;
                               g_ipage_frame_mode = getenv( "WASM_IPAGE_FRAME" ) ? 1 : 0; }
            if (tp_on)
            {
                double now = emscripten_get_now();
                if (tp_start == 0) { tp_start = tp_last = now; tp_last_insns = g_total_insns; tp_last_flip = g_flip_count; }
                else if (now - tp_last >= 1000.0)   /* sample once per wall-second */
                {
                    double dt = (now - tp_last) / 1000.0;
                    double fps = (double)(g_flip_count - tp_last_flip) / dt;
                    double mips = (double)(g_total_insns - tp_last_insns) / dt / 1e6;
#ifdef WEBWINE_GENBLOCKS
                    if (g_jit_on) {
                        uint64_t td = g_total_insns - tp_last_insns, jd = g_jit_insns - g_jit_last_insns;
                        fprintf( stderr, "JITCOV blocks/s=%llu jit_frac=%.1f%% total=%.1fM/s\n",
                            (unsigned long long)((g_jit_blocks - g_jit_last_blocks)),
                            td ? 100.0*(double)jd/(double)td : 0.0, (double)td/1e6/dt );
                        g_jit_last_insns = g_jit_insns; g_jit_last_blocks = g_jit_blocks; }
#endif
                    fprintf( stderr, "FPSSAMPLE t=%.1f flips=%llu fps=%.1f mips=%.1f loop/frame=%.0f audio=%.1fM/s calls=%llu vl=%.0f/%.0f mv=%.0f/%.0f mh=%.0f/%.0f v1=%.0f/%.0f mv1=%.0f/%.0f np2v=%.0f/%.0f np2m=%.0f/%.0f sb=%.0f/%.0f ld=%llu/%llu ldchk=%llu/%llu kinsn/frame=%.0f\n",
                             (now - tp_start) / 1000.0, (unsigned long long)g_flip_count, fps, mips,
                             fps > 0 ? (double)(g_count_hits - tp_last_hits) / fps : 0.0,
                             (double)(g_aud.cb_insns - tp_last_audio) / dt / 1e6,
                             (unsigned long long)(g_aud.cb_calls - tp_last_calls),
                             fps > 0 ? (double)(g_vl_calls - tp_last_vlc) / fps : 0.0,
                             fps > 0 ? (double)(g_vl_iters - tp_last_vli) / fps : 0.0,
                             fps > 0 ? (double)(g_mv_calls - tp_last_mvc) / fps : 0.0,
                             fps > 0 ? (double)(g_mv_iters - tp_last_mvi) / fps : 0.0,
                             fps > 0 ? (double)(g_mh_calls - tp_last_mhc) / fps : 0.0,
                             fps > 0 ? (double)(g_mh_iters - tp_last_mhi) / fps : 0.0,
                             fps > 0 ? (double)(g_v1_calls - tp_last_v1c) / fps : 0.0,
                             fps > 0 ? (double)(g_v1_iters - tp_last_v1i) / fps : 0.0,
                             fps > 0 ? (double)(g_mv1_calls - tp_last_mv1c) / fps : 0.0,
                             fps > 0 ? (double)(g_mv1_iters - tp_last_mv1i) / fps : 0.0,
                             fps > 0 ? (double)(g_v1n_calls - tp_last_v1nc) / fps : 0.0,
                             fps > 0 ? (double)(g_v1n_iters - tp_last_v1ni) / fps : 0.0,
                             fps > 0 ? (double)(g_mv1n_calls - tp_last_mv1nc) / fps : 0.0,
                             fps > 0 ? (double)(g_mv1n_iters - tp_last_mv1ni) / fps : 0.0,
                             fps > 0 ? (double)(g_sb_calls - tp_last_sbc) / fps : 0.0,
                             fps > 0 ? (double)(g_sb_iters - tp_last_sbi) / fps : 0.0,
                             (unsigned long long)g_ld_hits, (unsigned long long)g_ld_calls,
                             (unsigned long long)g_ld_ok, (unsigned long long)g_ld_bad,
                             fps > 0 ? (double)(g_total_insns - tp_last_insns) / fps / 1000.0 : 0.0 );
                    if (g_trace_3b)
                    { fprintf( stderr, "TRACE3B entries=%llu last=%08x\n",
                               (unsigned long long)g_trace_3b_hits, g_trace_3b_last );
                      g_trace_3b_hits = 0; }
                    if (g_glcount)
                    {
                        unsigned rank[8], nr = 0, s;
                        for (s = 0; s < NAT_SLOTS; s++) if (g_gl_calls[s])
                        {
                            unsigned p = nr < 8 ? nr++ : 8;
                            while (p && g_gl_calls[s] > g_gl_calls[rank[p-1]])
                            { if (p < 8) rank[p] = rank[p-1]; p--; }
                            if (p < 8) rank[p] = s;
                        }
                        fprintf( stderr, "GLCOUNT");
                        for (s = 0; s < nr; s++)
                            fprintf( stderr, " slot%u/code%u=%llu", rank[s], g_gl_code[rank[s]],
                                     (unsigned long long)g_gl_calls[rank[s]]);
                        fputc('\n', stderr);
                    }
                    tp_last_sbc = g_sb_calls; tp_last_sbi = g_sb_iters;
                    tp_last_mvc = g_mv_calls; tp_last_mvi = g_mv_iters;
                    tp_last_mhc = g_mh_calls; tp_last_mhi = g_mh_iters;
                    tp_last_v1c = g_v1_calls; tp_last_v1i = g_v1_iters;
                    tp_last_mv1c = g_mv1_calls; tp_last_mv1i = g_mv1_iters;
                    tp_last_v1nc = g_v1n_calls; tp_last_v1ni = g_v1n_iters;
                    tp_last_mv1nc = g_mv1n_calls; tp_last_mv1ni = g_mv1n_iters;
                    tp_last_vlc = g_vl_calls; tp_last_vli = g_vl_iters;
                    tp_last_hits = g_count_hits;
                    tp_last_audio = g_aud.cb_insns; tp_last_calls = g_aud.cb_calls;
                    tp_last = now; tp_last_insns = g_total_insns; tp_last_flip = g_flip_count;
                    if (g_prof_on && (int)((now - tp_start) / 1000.0) % 15 == 0) prof_dump();
                    if (g_histo_on && (int)((now - tp_start) / 1000.0) % 10 == 0)
                    {
                        int idx[512]; for (int i=0;i<512;i++) idx[i]=i;
                        for (int i=0;i<512;i++) for (int j=i+1;j<512;j++)
                            if (g_histo[idx[j]] > g_histo[idx[i]]) { int t=idx[i]; idx[i]=idx[j]; idx[j]=t; }
                        fprintf( stderr, "HISTO top:" );
                        for (int i=0;i<16;i++) { int o=idx[i]; if (!g_histo[o]) break;
                            fprintf( stderr, " %s%02x=%.1fM", o>=256?"0f":"", o&0xff, (double)g_histo[o]/1e6 ); }
                        fprintf( stderr, "\n" );
                    }
                    if (g_ipage_on)
                    {   /* top interpreted-entry pages: which 64KB of guest VA still
                         * runs in the interpreter (JIT missed there). */
                        unsigned pi[IPAGE_COUNT]; for (unsigned i=0;i<IPAGE_COUNT;i++) pi[i]=i;
                        qsort( pi, IPAGE_COUNT, sizeof(*pi), ipage_count_cmp );
                        uint64_t tot=0; for (int i=0;i<IPAGE_COUNT;i++) tot+=g_ipage[i];
                        fprintf( stderr, "IPAGE top (of %lluK entries):", (unsigned long long)(tot/1000) );
                        for (int i=0;i<12;i++) { int p=pi[i]; if (!g_ipage[p]) break;
                            fprintf( stderr, " %08x=%.1f%%", (unsigned)p<<16, 100.0*g_ipage[p]/(double)(tot?tot:1) ); }
                        fprintf( stderr, "\n" );
                        for (int i=0;i<12;i++) { int p=pi[i]; if (!g_ipage[p]) break;
                            uint32_t e = g_ipage_eip[p]; fprintf( stderr, "IPAGE bytes @%08x:", e-(uint32_t)nd_slide );
                            for (int k=0;k<160;k++) fprintf( stderr, "%02x", rd8( e+k ) );
                            fprintf( stderr, "\n" ); }
                        {   /* Exact miss entries turn a page hotspot into
                             * safe translator seeds for the next build. */
                            unsigned mi[20], nm = 0;
                            for (unsigned h=0; h<IMISS_COUNT; h++) if (g_imiss_n[h])
                            {
                                unsigned p = nm < 20 ? nm++ : 20;
                                while (p && g_imiss_n[h] > g_imiss_n[mi[p-1]])
                                { if (p < 20) mi[p] = mi[p-1]; p--; }
                                if (p < 20) mi[p] = h;
                            }
                            fprintf( stderr, "IMISS top:" );
                            for (unsigned i=0; i<nm; i++)
                                fprintf( stderr, " %08x=%.1fM", g_imiss_va[mi[i]],
                                         (double)g_imiss_n[mi[i]] / 1e6 );
                            fprintf( stderr, "\n" );
                            /* The M suffix hides the useful shape of runtime
                             * generated code: dozens of small blocks can each
                             * round to 0.0M while their aggregate page is hot.
                             * Keep the normal report compact, but expose exact
                             * counts on demand so a native/dynamic fast path
                             * can be selected from evidence. */
                            if (getenv( "WASM_IPAGE_DETAIL" ))
                            {
                                fprintf( stderr, "IMISS detail:" );
                                for (unsigned i=0; i<nm; i++)
                                    fprintf( stderr, " %08x=%llu", g_imiss_va[mi[i]],
                                             (unsigned long long)g_imiss_n[mi[i]] );
                                fprintf( stderr, "\n" );
                            }
                            /* The count alone cannot distinguish a tiny thunk
                             * from a real hot block.  Emit a short byte window
                             * for the leading entries so runtime-generated
                             * code can be classified without guessing from its
                             * allocation address.  This is diagnostic-only. */
                            for (unsigned i=0; i<nm && i<12; i++)
                            {
                                uint32_t e = g_imiss_va[mi[i]] + (uint32_t)nd_slide;
                                fprintf( stderr, "IMISS bytes @%08x:", g_imiss_va[mi[i]] );
                                for (unsigned k=0; k<32; k++) fprintf( stderr, "%02x", rd8( e+k ) );
                                fprintf( stderr, "\n" );
                            }
                        }
                    }
                }
            }
        }
        /* a native trampoline address (small wasm table index) -> back to C */
        if (c->eip < 0x10000)
        {
            if (c->eip == 0)
                fprintf( stderr, "wasm_x86: transfer to NULL from insn @ %08x "
                         "(esp=%08x [esp]=%08x [esp+4]=%08x eax=%08x ebx=%08x ecx=%08x edx=%08x)\n",
                         last_eip, c->regs[ESP], rd32(c->regs[ESP]), rd32(c->regs[ESP]+4),
                         c->regs[EAX], c->regs[EBX], c->regs[ECX], c->regs[EDX] );
            if (!wasm_x86_dispatch( c, c->eip ))
            {   /* Only the FATAL case gets a backtrace: transfers to NULL are
                 * routine here and mostly handled above, so an unconditional
                 * dump floods the log relay badly enough to stall the boot. */
                if (g_bt_left > 0 && getenv( "WASM_BT" ))
                {   /* Walk the %ebp chain: a call through a NULL pointer says nothing
                     * about who made it, and "the game froze" is all the page shows.
                     * Map the return addresses with the exe's symbols
                     * (VA = symbol value + 0x401000).
                     * OFF by default and hard-capped: reaching here is ROUTINE - the
                     * caller re-enters run() and carries on - so an unconditional dump
                     * floods the log relay badly enough to stall the boot.  Learned
                     * twice; hence both the env gate and the counter. */
                    uint32_t bp = c->regs[EBP];
                    g_bt_left--;
                    fprintf( stderr, "wasm_x86:   called from %08x\n", rd32( c->regs[ESP] ) );
                    for (int f = 0; f < 12 && bp >= 0x10000 && bp < 0xfff00000u; f++)
                    {
                        uint32_t ret = rd32( bp + 4 ), next = rd32( bp );
                        if (!ret) break;
                        fprintf( stderr, "wasm_x86:   frame %2d ret %08x\n", f, ret );
                        if (next <= bp) break;               /* chain must climb */
                        bp = next;
                    }
                }
                g_total_insns += idelta; return;   /* RUN_RETURN */
            }
            continue;
        }
        uint32_t start = c->eip;
        if (g_trace_3b < 0) g_trace_3b = getenv( "WASM_TRACE_3B" ) ? 1 : 0;
        if (g_trace_3b && start >= 0x3b821d50u && start < 0x3b821e10u)
        { g_trace_3b_hits++; g_trace_3b_last = start;
          if (g_trace_3b_detail < 0)
          { g_trace_3b_detail = getenv( "WASM_TRACE_3B_DETAIL" ) ? 1 : 0;
            g_trace_3b_detail_left = 12; }
          if (g_trace_3b_detail && g_trace_3b_detail_left)
          { uint32_t sp = c->regs[ESP];
            fprintf( stderr, "TRACE3B detail eip=%08x esp=%08x ret=%08x a0=%08x a1=%08x a2=%08x a3=%08x a4=%08x a5=%08x\n",
                     start, sp, rd32( sp ), rd32( sp + 4 ), rd32( sp + 8 ),
                     rd32( sp + 12 ), rd32( sp + 16 ), rd32( sp + 20 ), rd32( sp + 24 ) );
            g_trace_3b_detail_left--; }
        }
        {
            static int badip_on = -1;
            static unsigned badip_count;
            static uint32_t badip_last;
            uint32_t previous = last_eip;
            if (badip_on == -1) badip_on = getenv("WASM_BADIP") ? 1 : 0;
            if (badip_on && start < 0x400000u && previous >= 0x400000u &&
                badip_last != start && badip_count++ < 32) {
                badip_last = start;
                fprintf(stderr, "BADIP transition prev=%08x eip=%08x esp=%08x ebp=%08x eax=%08x ecx=%08x edx=%08x ret=%08x\n",
                        previous, start, c->regs[ESP], c->regs[EBP], c->regs[EAX],
                        c->regs[ECX], c->regs[EDX], rd32(c->regs[ESP]));
            }
        last_eip = start;
        }
#ifdef WEBWINE_GENBLOCKS
        if (g_jit_filling && g_verify_budget-- <= 0)
        {   /* verify mode: ran the block's instruction count -> stop BEFORE the
             * nat check, so a call to a hooked target does not run the callee. */
            g_total_insns += idelta; return;
        }
#endif
        if (nat_dynamic_jmp( c )) continue;
        if (g_wgl_swap_addr && start == g_wgl_swap_addr && nat_wglswap( c )) continue;
        /* Internal entries are the four instruction boundaries inside the
         * verified loop.  Match the small contiguous range directly: their
         * direct-dispatch slots can be occupied by unrelated runtime entries. */
        if (g_msvcrt_memset_loop && start > g_msvcrt_memset_loop &&
            start <= g_msvcrt_memset_loop + 12u && nat_memset_loop_tail( c )) continue;
        /* Frame boundary: _videoNextPage is the engine's page flip, entered once
         * per rendered frame with frameplace holding the COMPLETE frame.  Present
         * here (not on an insn-count timer) so the canvas shows whole, untorn
         * frames and the present rate equals the real render rate. */
        if (g_nat_addr[NAT_SLOT(start)] == start)
        {
            int kind = g_nat_kind[NAT_SLOT(start)];
            if (kind == NAT_COUNT) g_count_hits++;   /* diagnostic only: falls through */
            else if (kind != NAT_FLIP)
            {
                if (nat_call( c, kind )) continue;   /* eip set by the native call */
            }
            else {
            if (g_ipage_frame_mode && !g_ipage_frame_started)
            {   /* Startup contains large CRT/loader bursts.  Clear the
                 * diagnostic accumulators at the first real present so the
                 * next report describes render-time interpreter residue. */
                memset( g_ipage, 0, sizeof(g_ipage) );
                memset( g_ipage_eip, 0, sizeof(g_ipage_eip) );
                memset( g_imiss_va, 0, sizeof(g_imiss_va) );
                memset( g_imiss_n, 0, sizeof(g_imiss_n) );
                g_ipage_frame_started = 1;
            }
            g_flip_count++;
            /* Refill audio here rather than on the arbitrary housekeeping tick:
             * this is a clean guest function entry, whereas an arbitrary
             * instruction boundary can fall inside the guest's own heap or CRT
             * locks, and its critical sections are recursive for one thread - so
             * a mixer malloc could re-enter a half-updated allocator.  The frame
             * rate (~84/s) exceeds the buffer rate (~43/s), and the pump tops up
             * to the cushion in one visit, so this is also more than fast enough. */
            audio_pump( c );
#ifdef WEBWINE_BROWSER
            /* In OpenGL (Polymost) mode the picture is in the WebGL default
             * framebuffer and win32u presents it at the drawable swap;
             * frameplace is then a stale classic-renderer buffer, so presenting
             * it here would paint garbage over every GL frame. */
            { extern int webwine_gl_active;
              if (webwine_gl_active > 0) webwine_gl_active--;
              else wasm_dump_frame( c ); }
#else
            {   /* node headless capture: gated by WASM_DUMP_FRAME, up to 16 frames */
                static int dmp = -1, nframes = 0;
                if (dmp == -1) dmp = getenv( "WASM_DUMP_FRAME" ) ? 1 : 0;
                if (dmp && (g_flip_count % 20) == 0 && nframes < 16) { wasm_dump_frame( c ); nframes++; }
            }
#endif
            }
        }
#ifdef WEBWINE_GENBLOCKS
        if (g_jit_on && !g_jit_filling)
        {   /* run a whole AOT-translated block, if we have one for this eip */
#if defined(WEBWINE_FP_HOT)
            if (fp_hot_jit)
            {
                int fs = fp_hot_gen_lookup( start - (uint32_t)nd_slide );
                if (fs >= 0)
                {
                    if (g_jit_verify || fp_hot_verify)
                    { fp_hot_verify_block( c, fs, start ); continue; }
                    int fi = fp_hot_gen_idx[fs];
                    uint64_t fd = 0, fb = 0;
                    for (;;)
                    {
                        const struct fp_genblk *b = &fp_g_genblk[fi];
                        int nxt = b->fn( c );
                        fd += b->ninsn; fb++;
                        if (nxt < 0) break;
                        uint32_t ne = c->eip;
                        if (ne < 0x10000 || g_nat_addr[NAT_SLOT(ne)] == ne) break;
                        nxt = fp_hot_gen_lookup( ne - (uint32_t)nd_slide );
                        if (nxt < 0) break;
                        fi = fp_hot_gen_idx[nxt];
                    }
                    g_total_insns += fd; g_jit_insns += fd; g_jit_blocks += fb;
                    continue;
                }
            }
#endif
            int slot = gen_lookup_cached( start - (uint32_t)nd_slide );
            if (slot >= 0)
            {
                if (g_jit_verify)
                {   /* differential check: run JIT while logging writes, roll the
                     * memory back, then run the interpreter for the SAME number
                     * of guest instructions from an identical state and compare. */
                    struct x86cpu saved = *c;
                    g_wlog_n = 0; g_wlog_of = 0; g_jit_recording = 1;
                    g_gen_fn[slot]( c );
                    g_jit_recording = 0;
                    struct x86cpu jit = *c;
                    for (int i = g_wlog_n - 1; i >= 0; i--)   /* undo the JIT's writes */
                    { uint32_t a = g_wlog[i].addr;
                      if (g_wlog[i].size == 1) *(uint8_t*)(uintptr_t)a = (uint8_t)g_wlog[i].old;
                      else if (g_wlog[i].size == 2) *(uint16_t*)(uintptr_t)a = (uint16_t)g_wlog[i].old;
                      else *(uint32_t*)(uintptr_t)a = g_wlog[i].old; }
                    *c = saved;
                    if (!g_wlog_of)   /* skip blocks that overflowed the undo log */
                    {
                        g_verify_budget = g_gen_ninsn[slot];
                        c->running = 1; g_jit_filling = 1; run( c ); g_jit_filling = 0;
                        int bad = 0;
                        for (int r = 0; r < 8; r++) if (c->regs[r] != jit.regs[r])
                        { static int nb=0; bad=1; if (nb++<40)
                            fprintf( stderr, "JITBAD blk=%08x reg%d jit=%08x int=%08x eipj=%08x eipi=%08x\n",
                                     start-(uint32_t)nd_slide, r, jit.regs[r], c->regs[r],
                                     jit.eip-(uint32_t)nd_slide, c->eip-(uint32_t)nd_slide ); break; }
                        if (!bad) for (int r=0; r<8; r++) for (int k=0; k<16; k++)
                            if (c->xmm[r][k] != jit.xmm[r][k])
                            { static int nx=0; bad=1; if (nx++<40)
                                fprintf(stderr,"JITBAD blk=%08x xmm%d[%d] jit=%02x int=%02x eipj=%08x eipi=%08x\n",
                                    start-(uint32_t)nd_slide,r,k,jit.xmm[r][k],c->xmm[r][k],
                                    jit.eip-(uint32_t)nd_slide,c->eip-(uint32_t)nd_slide); break; }
                        if (!bad && c->eip != jit.eip) { static int nb=0; if (nb++<40)
                            fprintf( stderr, "JITBADEIP blk=%08x jit=%08x int=%08x\n",
                                     start-(uint32_t)nd_slide, jit.eip-(uint32_t)nd_slide, c->eip-(uint32_t)nd_slide ); }
                    }
                    else *c = jit;   /* couldn't verify; just keep the JIT result */
                    continue;
                }
                /* Tight JIT driver with block chaining: each block returns the
                 * g_genblk index of its statically-known successor, so we jump
                 * straight to it - no per-block hash lookup.  Break out only at a
                 * trampoline (eip<64K), a native hook boundary, a dynamic target
                 * (ret/indirect -> -1), or an untranslated target. */
                int idx = g_gen_idx[slot];
                uint64_t jd = 0, jb = 0;   /* accumulate in locals (wasm regs); the
                                            * per-block i64 memory traffic was pure
                                            * diagnostic overhead in the hot chain. */
                for (;;)
                {
                    const struct genblk *b = &g_genblk[idx];
                    int nxt = b->fn( c );
                    jd += b->ninsn; jb++;
                    if (nxt < 0) break;                             /* dynamic target */
                    uint32_t ne = c->eip;
                    if (ne < 0x10000) break;                        /* trampoline */
                    if (g_nat_addr[NAT_SLOT(ne)] == ne) break;      /* hook entry */
                    idx = nxt;
                }
                g_total_insns += jd; g_jit_insns += jd; g_jit_blocks += jb;
                continue;
            }
#if defined(WEBWINE_GENBLOCKS) && defined(WEBWINE_MSVCRT_AOT)
            /* Builtin msvcrt is relocatable and has its own AOT table.  Keep
             * this after the executable lookup and native-hook check: exported
             * memmove/memset/etc. must retain their proven host fast paths. */
            if (!g_jit_verify && g_msvcrt_jit && msvcrt_slide)
            {
                uint32_t mva = start - (uint32_t)msvcrt_slide;
                /* The current prototype is not permitted to enter the
                 * floating-point helper band that first exposed an x87/ABI
                 * mismatch; those functions stay on the interpreter path. */
                int ms = (mva < 0x1003e000u || mva >= 0x10041000u)
                       ? msvcrt_gen_lookup(mva) : -1;
                if (ms >= 0)
                {
                    int mi = msvcrt_gen_idx[ms];
                    uint64_t md = 0, mb = 0;
                    for (;;)
                    {
                        const struct msvcrt_genblk *b = &msvcrt_g_genblk[mi];
                        int nxt = b->fn(c);
                        md += b->ninsn; mb++;
                        if (nxt < 0) break;
                        uint32_t ne = c->eip;
                        if (ne < 0x10000 || g_nat_addr[NAT_SLOT(ne)] == ne) break;
                        mi = nxt;
                    }
                    g_total_insns += md; g_jit_insns += md; g_jit_blocks += mb;
                    continue;
                }
            }
#endif
#ifdef WEBWINE_GENBLOCKS
            /* GDI32's WidenPath is a measured frame hotspot.  Enter its tiny
             * relocatable candidate only when explicitly requested; all
             * dynamic calls and ungenerated blocks stay interpreter-owned. */
            if (!g_jit_verify && gdi32_jit && gdi32_slide)
            {
                int gs = gdi32_gen_lookup( start - (uint32_t)gdi32_slide );
                if (gs >= 0)
                {
                    int gi = gdi32_gen_idx[gs];
                    uint64_t gd = 0, gb = 0;
                    for (;;)
                    {
                        const struct gdi32_genblk *b = &gdi32_g_genblk[gi];
                        int nxt = b->fn( c );
                        gd += b->ninsn; gb++;
                        if (nxt < 0) break;
                        uint32_t ne = c->eip;
                        if (ne < 0x10000 || gdi32_gen_lookup( ne - (uint32_t)gdi32_slide ) < 0) break;
                        gi = gdi32_gen_idx[gdi32_gen_lookup( ne - (uint32_t)gdi32_slide )];
                    }
                    g_total_insns += gd; g_jit_insns += gd; g_jit_blocks += gb;
                    continue;
                }
            }
#endif
            /* JIT miss: this eip has no translated block.  Under WASM_IPAGE, tally
             * the interpreted block entry by 64KB guest page so we can see WHERE
             * the remaining interpreted load lives (which module/section). */
            if (g_ipage_on) { uint32_t va = start - (uint32_t)nd_slide;
                              unsigned pg = va >> 16;
                              unsigned h = (unsigned)((va * 2654435761u) >> (32 - 18));
                              g_ipage[pg]++; g_ipage_eip[pg] = start;
                              while (g_imiss_va[h] && g_imiss_va[h] != va)
                                  h = (h + 1) & (IMISS_COUNT - 1);
                              g_imiss_va[h] = va; g_imiss_n[h]++; }
            if (g_msvcrt_miss_trace == -1)
                g_msvcrt_miss_trace = getenv( "WASM_TRACE_MSVCRT_MISS" ) ? 1 : 0;
            if (g_msvcrt_miss_trace && start >= 0x3ee00000u && start < 0x3ee01000u &&
                g_msvcrt_miss_trace_n < 64)
            {
            fprintf( stderr, "MSVCRT_MISS eip=%08x prev=%08x esp=%08x ret=%08x args=%08x,%08x,%08x,%08x,%08x bytes=",
                     start, last_eip, c->regs[ESP], rd32(c->regs[ESP]),
                     rd32(c->regs[ESP] + 4), rd32(c->regs[ESP] + 8),
                     rd32(c->regs[ESP] + 12), rd32(c->regs[ESP] + 16),
                     rd32(c->regs[ESP] + 20) );
                for (unsigned ti = 0; ti < 16; ti++) fprintf( stderr, "%02x", rd8(start + ti) );
                fprintf( stderr, "\n" );
                g_msvcrt_miss_trace_n++;
            }
            if (g_msvcrt_hot_trace == -1)
                g_msvcrt_hot_trace = getenv( "WASM_TRACE_MSVCRT_HOT" ) ? 1 : 0;
            if (g_msvcrt_hot_trace &&
                ((start >= 0x3ee39ae0u && start < 0x3ee39ba0u) ||
                 (start >= 0x3ee4ff40u && start < 0x3ee4ffc0u)) &&
                g_msvcrt_hot_trace_n < 64)
            {
                fprintf( stderr, "MSVCRT_HOT eip=%08x prev=%08x esp=%08x ret=%08x regs=%08x,%08x,%08x,%08x,%08x,%08x\n",
                         start, last_eip, c->regs[ESP], rd32(c->regs[ESP]),
                         c->regs[EAX], c->regs[EBX], c->regs[ECX], c->regs[EDX],
                         c->regs[ESI], c->regs[EDI] );
                g_msvcrt_hot_trace_n++;
            }
            if (g_vline1_entry_trace == -1)
                g_vline1_entry_trace = getenv( "WASM_TRACE_VLINE1_ENTRY" ) ? 1 : 0;
            if (g_vline1_entry_trace && start >= 0x00631c90u &&
                start < 0x00631cf7u && g_vline1_entry_trace_n < 32)
            {
                fprintf( stderr,
                         "VLINE1_ENTRY eip=%08x prev=%08x esp=%08x ret=%08x "
                         "eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x "
                         "ebp=%08x stack=%08x,%08x,%08x,%08x,%08x,%08x\n",
                         start, last_eip, c->regs[ESP], rd32(c->regs[ESP]),
                         c->regs[EAX], c->regs[EBX], c->regs[ECX], c->regs[EDX],
                         c->regs[ESI], c->regs[EDI], c->regs[EBP],
                         rd32(c->regs[ESP] + 4), rd32(c->regs[ESP] + 8),
                         rd32(c->regs[ESP] + 12), rd32(c->regs[ESP] + 16),
                         rd32(c->regs[ESP] + 20), rd32(c->regs[ESP] + 24) );
                g_vline1_entry_trace_n++;
            }
        }
#endif
        struct decode d = { 4, 4, 0, 0, c->eip };
        uint8_t op;

        /* prefixes */
        /* Classify the byte via a table (0 = not a prefix) so the common
         * no-prefix case costs one lookup + branch instead of ~11 compares. */
        for (;;)
        {
            op = f8( &d );
            uint8_t pc = prefix_class[op];
            if (!pc) break;
            switch (pc)
            {
            case 1: d.opsize = 2; break;                 /* 66 */
            case 2: d.addrsize = 2; break;               /* 67 */
            case 3: d.rep = op; break;                   /* f2/f3 */
            case 4: d.seg = (int)c->fs_base; break;      /* 64 fs -> TEB */
            case 5: d.seg = (int)c->gs_base; break;      /* 65 gs */
            case 6: d.seg = 0; break;                    /* 26/2e/36/3e flat */
            default: break;                              /* 7 = f0 lock (no-op) */
            }
        }

        int os = d.opsize;
        struct modrm m;
        uint32_t a, b, r;

        if (g_histo_on) g_histo[op]++;
        switch (op)
        {
        case 0x88: m = decode_modrm(c,&d); write_rm(c,&m,1, read_reg(c,m.reg,1)); break;
        case 0x89: m = decode_modrm(c,&d); write_rm(c,&m,os, read_reg(c,m.reg,os)); break;
        case 0x8a: m = decode_modrm(c,&d); write_reg(c,m.reg,1, read_rm(c,&m,1)); break;
        case 0x8b: m = decode_modrm(c,&d); write_reg(c,m.reg,os, read_rm(c,&m,os)); break;
        case 0x8d: m = decode_modrm(c,&d); write_reg(c,m.reg,os, m.ea); break; /* lea */
        /* MOV r/m16, Sreg — store the segment selector.  We run a flat model
         * (bases in fs_base/gs_base), but code reads/stores the selector values;
         * hand back Wine's standard i386 user selectors.  reg field: 0=ES 1=CS
         * 2=SS 3=DS 4=FS 5=GS. */
        case 0x8c: m = decode_modrm(c,&d);
        {
            static const uint16_t sel[6] = { 0x2b, 0x23, 0x2b, 0x2b, 0x3b, 0x63 };
            uint16_t s = (m.reg < 6) ? sel[m.reg] : 0;
            write_rm( c, &m, m.is_reg ? os : 2, s );
        } break;
        /* MOV Sreg, r/m16 — accept the load; the flat bases are fixed (TEB is
         * pinned), so changing a selector is a no-op here. */
        case 0x8e: m = decode_modrm(c,&d); (void)read_rm(c,&m,2); break;
        case 0xc6: m = decode_modrm(c,&d); { uint8_t im=f8(&d); write_rm(c,&m,1,im); } break;
        case 0xc7: m = decode_modrm(c,&d); { uint32_t im = os==2?f16(&d):f32(&d); write_rm(c,&m,os,im); } break;
        case 0xb0: case 0xb1: case 0xb2: case 0xb3: case 0xb4: case 0xb5: case 0xb6: case 0xb7:
            write_reg(c, op&7, 1, f8(&d)); break;
        case 0xb8: case 0xb9: case 0xba: case 0xbb: case 0xbc: case 0xbd: case 0xbe: case 0xbf:
            write_reg(c, op&7, os, os==2?f16(&d):f32(&d)); break;

        /* push/pop */
        case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
            push32(c, c->regs[op&7]); break;
        case 0x58: case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f:
            c->regs[op&7] = pop32(c); break;
        case 0x68: push32(c, f32(&d)); break;
        case 0x6a: push32(c, (int32_t)(int8_t)f8(&d)); break;
        case 0xff: m = decode_modrm(c,&d);
            switch (m.reg)
            {
            case 0: a = read_rm(c,&m,os); r = a+1; write_rm(c,&m,os,r); set_lazy(c,K_INC,a,1,r,os); break; /* inc */
            case 1: a = read_rm(c,&m,os); r = a-1; write_rm(c,&m,os,r); set_lazy(c,K_DEC,a,1,r,os); break; /* dec */
            case 2: a = read_rm(c,&m,os); push32(c, d.eip); c->eip = a; goto next; /* call r/m */
            case 4: a = read_rm(c,&m,os); c->eip = a; goto next; /* jmp r/m */
            case 6: push32(c, read_rm(c,&m,os)); break; /* push r/m */
            default: unimplemented(c,start,op); g_total_insns += idelta; return;   /* RUN_RETURN */
            }
            break;
        case 0x8f: m = decode_modrm(c,&d); write_rm(c,&m,os, pop32(c)); break;
        case 0x9c: push32(c, get_flags(c)); break;           /* pushfd */
        case 0x9d: c->eflags = pop32(c); c->lf_size = 0; break; /* popfd */
        case 0xfe: m = decode_modrm(c,&d); /* grp4: inc/dec r/m8 */
            switch (m.reg) {
            case 0: a = read_rm(c,&m,1); r = (a+1)&0xff; write_rm(c,&m,1,r); set_lazy(c,K_INC,a,1,r,1); break; /* inc */
            case 1: a = read_rm(c,&m,1); r = (a-1)&0xff; write_rm(c,&m,1,r); set_lazy(c,K_DEC,a,1,r,1); break; /* dec */
            default: unimplemented(c,start,op); g_total_insns += idelta; return;   /* RUN_RETURN */
            }
            break;

        /* arithmetic families: opcodes xx0..xx5 (reg8, reg, r/m8<-r, r/m<-r, r<-r/m8, r<-r/m, al,imm8, eax,imm) */
#define ARITH(base, KIND, EXPR) \
        case base+0: m=decode_modrm(c,&d); a=read_rm(c,&m,1); b=read_reg(c,m.reg,1); r=(EXPR); write_rm(c,&m,1,r); set_lazy(c,KIND,a,b,r,1); break; \
        case base+1: m=decode_modrm(c,&d); a=read_rm(c,&m,os); b=read_reg(c,m.reg,os); r=(EXPR); write_rm(c,&m,os,r); set_lazy(c,KIND,a,b,r,os); break; \
        case base+2: m=decode_modrm(c,&d); a=read_reg(c,m.reg,1); b=read_rm(c,&m,1); r=(EXPR); write_reg(c,m.reg,1,r); set_lazy(c,KIND,a,b,r,1); break; \
        case base+3: m=decode_modrm(c,&d); a=read_reg(c,m.reg,os); b=read_rm(c,&m,os); r=(EXPR); write_reg(c,m.reg,os,r); set_lazy(c,KIND,a,b,r,os); break; \
        case base+4: a=read_reg(c,EAX,1); b=f8(&d); r=(EXPR); write_reg(c,EAX,1,r); set_lazy(c,KIND,a,b,r,1); break; \
        case base+5: a=read_reg(c,EAX,os); b=os==2?f16(&d):f32(&d); r=(EXPR); write_reg(c,EAX,os,r); set_lazy(c,KIND,a,b,r,os); break;

        ARITH(0x00, K_ADD, a+b)                 /* add */
        ARITH(0x08, K_LOGIC, a|b)               /* or  */
        ARITH(0x20, K_LOGIC, a&b)               /* and */
        ARITH(0x28, K_SUB, a-b)                 /* sub */
        ARITH(0x30, K_LOGIC, a^b)               /* xor */
        /* cmp: like sub but no writeback */
        case 0x38: m=decode_modrm(c,&d); a=read_rm(c,&m,1); b=read_reg(c,m.reg,1); set_lazy(c,K_SUB,a,b,a-b,1); break;
        case 0x39: m=decode_modrm(c,&d); a=read_rm(c,&m,os); b=read_reg(c,m.reg,os); set_lazy(c,K_SUB,a,b,a-b,os); break;
        case 0x3a: m=decode_modrm(c,&d); a=read_reg(c,m.reg,1); b=read_rm(c,&m,1); set_lazy(c,K_SUB,a,b,a-b,1); break;
        case 0x3b: m=decode_modrm(c,&d); a=read_reg(c,m.reg,os); b=read_rm(c,&m,os); set_lazy(c,K_SUB,a,b,a-b,os); break;
        case 0x3c: a=read_reg(c,EAX,1); b=f8(&d); set_lazy(c,K_SUB,a,b,a-b,1); break;
        case 0x3d: a=read_reg(c,EAX,os); b=os==2?f16(&d):f32(&d); set_lazy(c,K_SUB,a,b,a-b,os); break;

        /* grp1: 80/81/83 r/m, imm */
        case 0x80: case 0x81: case 0x83:
        {
            m = decode_modrm(c,&d);
            int sz = (op==0x80)?1:os;
            a = read_rm(c,&m,sz);
            if (op==0x80) b = f8(&d);
            else if (op==0x83) b = (int32_t)(int8_t)f8(&d);
            else b = (os==2)?f16(&d):f32(&d);
            b &= sizemask(sz);
            switch (m.reg)
            {
            case 0: r=a+b; write_rm(c,&m,sz,r); set_lazy(c,K_ADD,a,b,r,sz); break;
            case 1: r=a|b; write_rm(c,&m,sz,r); set_lazy(c,K_LOGIC,a,b,r,sz); break;
            case 4: r=a&b; write_rm(c,&m,sz,r); set_lazy(c,K_LOGIC,a,b,r,sz); break;
            case 5: r=a-b; write_rm(c,&m,sz,r); set_lazy(c,K_SUB,a,b,r,sz); break;
            case 6: r=a^b; write_rm(c,&m,sz,r); set_lazy(c,K_LOGIC,a,b,r,sz); break;
            case 7: set_lazy(c,K_SUB,a,b,a-b,sz); break; /* cmp */
            case 2: { int ci=lf_cf(c); r=a+b+ci; write_rm(c,&m,sz,r); set_lazy(c,K_ADC,a,b,r,sz); c->lf_cin=ci; } break; /* adc */
            case 3: { int ci=lf_cf(c); r=a-b-ci; write_rm(c,&m,sz,r); set_lazy(c,K_SBB,a,b,r,sz); c->lf_cin=ci; } break; /* sbb */
            }
            break;
        }

        /* test */
        case 0x84: m=decode_modrm(c,&d); a=read_rm(c,&m,1); b=read_reg(c,m.reg,1); set_lazy(c,K_LOGIC,a,b,a&b,1); break;
        case 0x85: m=decode_modrm(c,&d); a=read_rm(c,&m,os); b=read_reg(c,m.reg,os); set_lazy(c,K_LOGIC,a,b,a&b,os); break;
        case 0xa8: a=read_reg(c,EAX,1); b=f8(&d); set_lazy(c,K_LOGIC,a,b,a&b,1); break;
        case 0xa9: a=read_reg(c,EAX,os); b=os==2?f16(&d):f32(&d); set_lazy(c,K_LOGIC,a,b,a&b,os); break;

        /* inc/dec reg */
        case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
            a=c->regs[op&7]; r=a+1; c->regs[op&7]=r; set_lazy(c,K_INC,a,1,r,os); break;
        case 0x48: case 0x49: case 0x4a: case 0x4b: case 0x4c: case 0x4d: case 0x4e: case 0x4f:
            a=c->regs[op&7]; r=a-1; c->regs[op&7]=r; set_lazy(c,K_DEC,a,1,r,os); break;

        /* xchg */
        case 0x86: m=decode_modrm(c,&d); a=read_rm(c,&m,1); b=read_reg(c,m.reg,1); write_rm(c,&m,1,b); write_reg(c,m.reg,1,a); break;
        case 0x87: m=decode_modrm(c,&d); a=read_rm(c,&m,os); b=read_reg(c,m.reg,os); write_rm(c,&m,os,b); write_reg(c,m.reg,os,a); break;
        case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
            a=c->regs[EAX]; c->regs[EAX]=c->regs[op&7]; c->regs[op&7]=a; break;
        case 0x90: break; /* nop / xchg eax,eax */
        case 0x9b: break; /* fwait/wait: no async x87 exceptions here -> no-op */
        case 0xfa: case 0xfb: break; /* cli/sti: no guest interrupt controller */
        case 0x9e: /* sahf: AH -> SF ZF AF PF CF (AH mirrors the low EFLAGS byte) */
            { uint8_t ah = (c->regs[EAX] >> 8) & 0xff; c->eflags = get_flags(c);
              c->eflags = (c->eflags & ~(SF|ZF|AF|PF|CF)) | (ah & (SF|ZF|AF|PF|CF)); c->lf_size = 0; }
            break;
        case 0x9f: /* lahf: SF ZF AF PF CF -> AH (bit1 always set) */
            { uint32_t fl = get_flags(c);
              c->regs[EAX] = (c->regs[EAX] & ~0xff00u) | ((((fl & (SF|ZF|AF|PF|CF)) | 0x02) & 0xff) << 8); }
            break;

        /* control flow */
        case 0xe8: { int32_t rel = f32(&d); push32(c, d.eip); c->eip = d.eip + rel; goto next; } /* call rel */
        case 0xe9: { int32_t rel = f32(&d); c->eip = d.eip + rel; goto next; }
        case 0xeb: { int32_t rel = (int8_t)f8(&d); c->eip = d.eip + rel; goto next; }
        case 0xc3: c->eip = pop32(c); goto next;                 /* ret */
        case 0xc2: { uint16_t n=f16(&d); r=pop32(c); c->regs[ESP]+=n; c->eip=r; goto next; }
        case 0xc9: c->regs[ESP]=c->regs[EBP]; c->regs[EBP]=pop32(c); break; /* leave */

        /* jcc rel8 */
        case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7a: case 0x7b: case 0x7c: case 0x7d: case 0x7e: case 0x7f:
        { int32_t rel=(int8_t)f8(&d); if (cond(c, op-0x70)) { c->eip = d.eip + rel; goto next; } break; }

        case 0x98: /* cwde */ c->regs[EAX] = (int32_t)(int16_t)(c->regs[EAX] & 0xffff); break;
        case 0x99: /* cdq */ c->regs[EDX] = (c->regs[EAX] & 0x80000000) ? 0xffffffff : 0; break;

        /* mov al/eax <-> moffs32 (honor segment base: fs->TEB, else flat) */
        case 0xa0: { uint32_t off=f32(&d)+(uint32_t)d.seg; c->regs[EAX]=(c->regs[EAX]&0xffffff00)|rd8(off); } break;
        case 0xa1: { uint32_t off=f32(&d)+(uint32_t)d.seg; write_reg(c,EAX,os, os==2?rd16(off):rd32(off)); } break;
        case 0xa2: { uint32_t off=f32(&d)+(uint32_t)d.seg; wr8(off, c->regs[EAX]&0xff); } break;
        case 0xa3: { uint32_t off=f32(&d)+(uint32_t)d.seg; if(os==2) wr16(off,c->regs[EAX]); else wr32(off,c->regs[EAX]); } break;

        /* adc / sbb (reg forms) */
        /* adc/sbb: cold, see run_cold */
        case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15:
        case 0x18: case 0x19: case 0x1a: case 0x1b: case 0x1c: case 0x1d:
            run_cold( c, &d, op );
            break;

        /* pushad / popad */
        case 0x60: { uint32_t sp=c->regs[ESP]; push32(c,c->regs[EAX]); push32(c,c->regs[ECX]); push32(c,c->regs[EDX]); push32(c,c->regs[EBX]); push32(c,sp); push32(c,c->regs[EBP]); push32(c,c->regs[ESI]); push32(c,c->regs[EDI]); } break;
        case 0x61: c->regs[EDI]=pop32(c); c->regs[ESI]=pop32(c); c->regs[EBP]=pop32(c); pop32(c); c->regs[EBX]=pop32(c); c->regs[EDX]=pop32(c); c->regs[ECX]=pop32(c); c->regs[EAX]=pop32(c); break;

        /* imul r, r/m, imm */
        case 0x69: m=decode_modrm(c,&d); a=read_rm(c,&m,os); b=(os==2?f16(&d):f32(&d)); write_reg(c,m.reg,os,(int32_t)a*(int32_t)b); break;
        case 0x6b: m=decode_modrm(c,&d); a=read_rm(c,&m,os); b=(int32_t)(int8_t)f8(&d); write_reg(c,m.reg,os,(int32_t)a*(int32_t)b); break;

        /* shift/rotate group: C0/C1 (imm8), D0/D1 (by 1), D2/D3 (by cl) */
        /* shifts/rotates: cold, see run_cold */
        case 0xc0: case 0xc1: case 0xd0: case 0xd1: case 0xd2: case 0xd3:
            run_cold( c, &d, op );
            break;

        /* grp3: test/not/neg are cheap AND hot - the engine's per-frame sprite
         * walk in videoNextPage runs "testb $imm,disp(%eax)" every iteration, so
         * sending 0xf6 to run_cold cost a call per iteration of the hottest loop
         * in the frame.  Only the bulky mul/imul/div/idiv arms stay cold. */
        case 0xf6: case 0xf7:
        {
            int sz = (op&1) ? os : 1;
            m = decode_modrm(c,&d);
            if (m.reg >= 4) { run_cold_grp3( c, &d, op, &m, sz ); break; }
            a = read_rm(c,&m,sz);
            switch (m.reg)
            {
            case 0: case 1: b = (sz==1) ? f8(&d) : (os==2 ? f16(&d) : f32(&d));
                            set_lazy(c,K_LOGIC,a,b,a&b,sz); break;   /* test */
            case 2: write_rm(c,&m,sz,~a); break;                     /* not */
            default: r = (uint32_t)(-(int32_t)a); write_rm(c,&m,sz,r);
                     set_lazy(c,K_SUB,0,a,r,sz); break;              /* neg */
            }
            break;
        }

        /* string ops: cold, see run_cold */
        case 0xa4: case 0xa5:
        case 0xaa: case 0xab:
        case 0xac: case 0xad:
            run_cold( c, &d, op );
            break;
        case 0xf5: c->eflags ^= CF; break;  /* cmc */
        case 0xf8: c->eflags &= ~CF; break;  /* clc */
        case 0xf9: c->eflags |= CF; break;   /* stc */
        case 0xfc: c->eflags &= ~DF; break;  /* cld */
        case 0xfd: c->eflags |= DF; break;   /* std */

        /* ---- x87 FPU (0xd8-0xdf) ---- */
        case 0xd8: case 0xd9: case 0xda: case 0xdb:
        case 0xdc: case 0xdd: case 0xde: case 0xdf:
            run_x87( c, &d, op );   /* cold: see run_x87 */
            break;
        case 0x0f: /* two-byte */
        {
            uint8_t op2 = f8(&d);
            if (g_histo_on) g_histo[256 + op2]++;
            if (op2 >= 0x80 && op2 <= 0x8f) /* jcc rel32 */
            { int32_t rel=f32(&d); if (cond(c, op2-0x80)) { c->eip=d.eip+rel; goto next; } break; }
            if (op2 >= 0x90 && op2 <= 0x9f) /* setcc */
            { m=decode_modrm(c,&d); write_rm(c,&m,1, cond(c,op2-0x90)?1:0); break; }
            if (op2 >= 0x40 && op2 <= 0x4f) /* cmovcc */
            { m=decode_modrm(c,&d); a=read_rm(c,&m,os); if (cond(c,op2-0x40)) write_reg(c,m.reg,os,a); break; }
            switch (op2)
            {
            case 0xb6: m=decode_modrm(c,&d); write_reg(c,m.reg,os, read_rm(c,&m,1)); break;       /* movzx r,rm8 */
            case 0xb7: m=decode_modrm(c,&d); write_reg(c,m.reg,os, read_rm(c,&m,2)); break;       /* movzx r,rm16 */
            case 0xbe: m=decode_modrm(c,&d); write_reg(c,m.reg,os, (int32_t)(int8_t)read_rm(c,&m,1)); break;
            case 0xbf: m=decode_modrm(c,&d); write_reg(c,m.reg,os, (int32_t)(int16_t)read_rm(c,&m,2)); break;
            case 0xaf: m=decode_modrm(c,&d); a=read_reg(c,m.reg,os); b=read_rm(c,&m,os); write_reg(c,m.reg,os,a*b); break; /* imul */

            /* ---- SSE/SSE2 (enough for memset/memcpy/strlen in ntdll) ---- */
            default:   /* SSE/MMX/bit-ops live in run_sse (cold, see there) */
                if (!run_sse( c, &d, op2 )) { unimplemented(c,start,0x0f); g_total_insns += idelta; return; }   /* RUN_RETURN */
                break;
            }
            break;
        }

        default:
            unimplemented( c, start, op );
            g_total_insns += idelta;   /* RUN_RETURN */
            return;
        }
        c->eip = d.eip;
    next: ;
    }
}

/* ---- public entry from the ntdll seam ---- */

void wasm_x86_run_context( CONTEXT *ctx )
{
    struct x86cpu *c = &g_cpu;
    c->regs[EAX]=ctx->Eax; c->regs[ECX]=ctx->Ecx; c->regs[EDX]=ctx->Edx; c->regs[EBX]=ctx->Ebx;
    c->regs[ESP]=ctx->Esp; c->regs[EBP]=ctx->Ebp; c->regs[ESI]=ctx->Esi; c->regs[EDI]=ctx->Edi;
    c->eip=ctx->Eip; c->eflags=ctx->EFlags?ctx->EFlags:0x202; c->lf_size=0;
    fprintf( stderr, "wasm_x86: starting guest at eip=%08x esp=%08x\n", c->eip, c->regs[ESP] );
    run( c );
    fprintf( stderr, "wasm_x86: guest run stopped (eip=%08x exit=%x)\n", c->eip, c->exit_code );
}

void wasm_x86_save_context( CONTEXT *ctx )
{
    struct x86cpu *c = &g_cpu;
    ctx->Eax=c->regs[EAX]; ctx->Ecx=c->regs[ECX]; ctx->Edx=c->regs[EDX]; ctx->Ebx=c->regs[EBX];
    ctx->Esp=c->regs[ESP]; ctx->Ebp=c->regs[EBP]; ctx->Esi=c->regs[ESI]; ctx->Edi=c->regs[EDI];
    ctx->Eip=c->eip; ctx->EFlags=get_flags(c);
}

/* ---- syscall dispatch seam ---- */

extern SYSTEM_SERVICE_TABLE KeServiceDescriptorTable[4];

/* wasm cannot call a function pointer with a variable arg count, so provide one
 * trampoline per dword-arg count. i386 stdcall passes every arg as a stack
 * dword; on wasm32 each maps to one i32 param, matching handlers whose args are
 * all pointer/32-bit-wide (the common case for Nt* syscalls). */
typedef uint32_t (*fn0)(void);
typedef uint32_t (*fn1)(uint32_t);
typedef uint32_t (*fn2)(uint32_t,uint32_t);
typedef uint32_t (*fn3)(uint32_t,uint32_t,uint32_t);
typedef uint32_t (*fn4)(uint32_t,uint32_t,uint32_t,uint32_t);
typedef uint32_t (*fn5)(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
typedef uint32_t (*fn6)(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
typedef uint32_t (*fn7)(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
typedef uint32_t (*fn8)(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
typedef uint32_t (*fn9)(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
typedef uint32_t (*fn10)(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
typedef uint32_t (*fn11)(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
typedef uint32_t (*fn12)(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
#define WU uint32_t
typedef WU (*fn13)(WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU);
typedef WU (*fn14)(WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU);
typedef WU (*fn15)(WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU);
typedef WU (*fn16)(WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU);
typedef WU (*fn17)(WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU);
typedef WU (*fn18)(WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU);
typedef WU (*fn19)(WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU);
typedef WU (*fn20)(WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU,WU);
#undef WU

static uint32_t call_handler( void *fn, int nargs, uint32_t *a )
{
    switch (nargs)
    {
    case 0: return ((fn0)fn)();
    case 1: return ((fn1)fn)(a[0]);
    case 2: return ((fn2)fn)(a[0],a[1]);
    case 3: return ((fn3)fn)(a[0],a[1],a[2]);
    case 4: return ((fn4)fn)(a[0],a[1],a[2],a[3]);
    case 5: return ((fn5)fn)(a[0],a[1],a[2],a[3],a[4]);
    case 6: return ((fn6)fn)(a[0],a[1],a[2],a[3],a[4],a[5]);
    case 7: return ((fn7)fn)(a[0],a[1],a[2],a[3],a[4],a[5],a[6]);
    case 8: return ((fn8)fn)(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7]);
    case 9: return ((fn9)fn)(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],a[8]);
    case 10: return ((fn10)fn)(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],a[8],a[9]);
    case 11: return ((fn11)fn)(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],a[8],a[9],a[10]);
    case 12: return ((fn12)fn)(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],a[8],a[9],a[10],a[11]);
    case 13: return ((fn13)fn)(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],a[8],a[9],a[10],a[11],a[12]);
    case 14: return ((fn14)fn)(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],a[8],a[9],a[10],a[11],a[12],a[13]);
    case 15: return ((fn15)fn)(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],a[8],a[9],a[10],a[11],a[12],a[13],a[14]);
    case 16: return ((fn16)fn)(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],a[8],a[9],a[10],a[11],a[12],a[13],a[14],a[15]);
    case 17: return ((fn17)fn)(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],a[8],a[9],a[10],a[11],a[12],a[13],a[14],a[15],a[16]);
    case 18: return ((fn18)fn)(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],a[8],a[9],a[10],a[11],a[12],a[13],a[14],a[15],a[16],a[17]);
    case 19: return ((fn19)fn)(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],a[8],a[9],a[10],a[11],a[12],a[13],a[14],a[15],a[16],a[17],a[18]);
    case 20: return ((fn20)fn)(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],a[8],a[9],a[10],a[11],a[12],a[13],a[14],a[15],a[16],a[17],a[18],a[19]);
    default:
        fprintf( stderr, "wasm_x86: call_handler unsupported nargs=%d\n", nargs );
        return ((fn20)fn)(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],a[8],a[9],a[10],a[11],a[12],a[13],a[14],a[15],a[16],a[17],a[18],a[19]);
    }
}

/* A handful of win32u syscalls return void, so their wasm signature is
 * (N i32)->() rather than (N i32)->i32; calling them through call_handler's
 * i32-returning function pointer traps with "function signature mismatch".
 * Dispatch those through void-typed pointers (the guest ignores EAX). */
typedef void (*fv2)(uint32_t,uint32_t);
typedef void (*fv4)(uint32_t,uint32_t,uint32_t,uint32_t);
static void call_handler_void( void *fn, int nargs, uint32_t *a )
{
    switch (nargs)
    {
    case 2: ((fv2)fn)(a[0],a[1]); break;
    default: ((fv4)fn)(a[0],a[1],a[2],a[3]); break;  /* the void syscalls take 2 or 4 dwords */
    }
}
/* win32u syscall ids whose handler returns void (see win32syscalls.h) */
static int syscall_returns_void( uint32_t num )
{
    return num == 0x14be   /* NtUserNotifyIMEStatus */
        || num == 0x14c1   /* NtUserNotifyWinEvent */
        || num == 0x1564;  /* NtUserSetInternalWindowPos */
}

static uint32_t addr_syscall, addr_apc, addr_exc, addr_cb;

/* ---- user-mode callbacks (win32u -> user32 window procs etc.) ---- */
static void *g_cb_ret_ptr;
static uint32_t g_cb_ret_len;
static int g_cb_status;

/* KeUserModeCallback: enter pKiUserCallbackDispatcher on the guest stack with a
 * callback_stack_layout (size 0x1c), run the interpreter re-entrantly until the
 * guest calls NtCallbackReturn, then restore and return the result. */
NTSTATUS wasm_x86_user_callback( uint32_t id, const void *args, uint32_t len, void **ret_ptr, uint32_t *ret_len )
{
    struct x86cpu *c = &g_cpu;
    struct x86cpu saved = *c;
    uint32_t old_esp = c->regs[ESP];
    uint32_t esp = (old_esp - (0x1c + len)) & ~3u;

    wr32( esp + 0x00, c->eip );          /* eip (resume marker) */
    wr32( esp + 0x04, id );
    wr32( esp + 0x08, esp + 0x1c );      /* args -> args_data */
    wr32( esp + 0x0c, len );
    wr32( esp + 0x10, 0 ); wr32( esp + 0x14, 0 );
    wr32( esp + 0x18, old_esp );         /* saved esp */
    if (len) memcpy( (void *)(uintptr_t)(esp + 0x1c), args, len );

    c->regs[ESP] = esp;
    c->regs[EBP] = 0;
    c->eip = addr_cb;                    /* pKiUserCallbackDispatcher */
    g_cb_status = STATUS_NO_CALLBACK_ACTIVE;
    g_cb_ret_ptr = NULL; g_cb_ret_len = 0;

    run( c );                            /* re-entrant; NtCallbackReturn stops it */

    if (ret_ptr) *ret_ptr = g_cb_ret_ptr;
    if (ret_len) *ret_len = g_cb_ret_len;
    { NTSTATUS st = (NTSTATUS)g_cb_status; *c = saved; return st; }
}

/* NtCallbackReturn body: capture the result and unwind the re-entrant run(). */
NTSTATUS wasm_x86_callback_return( void *ret_ptr, uint32_t ret_len, int status )
{
    g_cb_ret_ptr = ret_ptr; g_cb_ret_len = ret_len; g_cb_status = status;
    g_cpu.running = 0;
    return (NTSTATUS)status;
}

/* ---- exception delivery to the guest (SEH) ----------------------------------
 * The unix side (via NtRaiseException / a fault) hands us an EXCEPTION_RECORD +
 * CONTEXT to deliver to user mode.  Mirror i386 call_user_exception_dispatcher:
 * build { rec_ptr; ctx_ptr; rec; ctx } on the guest stack and enter
 * pKiUserExceptionDispatcher (which reads rec_ptr at [esp], ctx_ptr at [esp+4]).
 * A handler resumes via NtContinue (already handled below).  Set a flag so the
 * syscall dispatcher doesn't overwrite the new eip with the syscall return. */
int g_wasm_exc_pending;
void wasm_x86_setup_exception( EXCEPTION_RECORD *rec, CONTEXT *ctx )
{
    struct x86cpu *c = &g_cpu;
    uint32_t sr = (uint32_t)sizeof(EXCEPTION_RECORD);
    uint32_t sc = (uint32_t)sizeof(CONTEXT);
    uint32_t stack = (c->regs[ESP] - (8 + sr + sc)) & ~15u;
    uint32_t rec_ptr = stack + 8;
    uint32_t ctx_ptr = stack + 8 + sr;
    wr32( stack + 0, rec_ptr );
    wr32( stack + 4, ctx_ptr );
    memcpy( (void *)(uintptr_t)rec_ptr, rec, sr );
    memcpy( (void *)(uintptr_t)ctx_ptr, ctx, sc );
    if (rec->ExceptionCode == 0x80000003 /* EXCEPTION_BREAKPOINT */)
        wr32( ctx_ptr + 0xb8, rd32( ctx_ptr + 0xb8 ) - 1 );   /* context->Eip-- */
    c->regs[ESP] = stack;
    c->eip = addr_exc;
    g_wasm_exc_pending = 1;
    if (trace()) fprintf( stderr, "wasm_x86: deliver exception code=%08x -> KiUserExceptionDispatcher esp=%08x\n",
                          (unsigned)rec->ExceptionCode, stack );
}

int wasm_x86_dispatch( struct x86cpu *c, uint32_t target )
{
    if (target == GUEST_RET_SENTINEL)            /* end of a call_guest_cdecl() */
    {
        c->running = 0;
        return 0;
    }
    if (target == 0xdeadbabe || target == 0)
    {
        fprintf( stderr, "wasm_x86: guest returned to top-level (%08x) — thread exit\n", target );
        c->running = 0; return 0;
    }
    if (target == addr_syscall)
    {
        uint32_t ret_eip = pop32( c );            /* return into the Nt stub */
        uint32_t num = c->regs[EAX];
        int table = (num >> 12) & 3, idx = num & 0xfff;
        SYSTEM_SERVICE_TABLE *t = &KeServiceDescriptorTable[table];
        if ((ULONG_PTR)idx >= t->ServiceLimit)
        { c->regs[EAX] = STATUS_INVALID_SYSTEM_SERVICE; c->eip = ret_eip; return 1; }
        void *fn = (void *)t->ServiceTable[idx];
        int nargs = t->ArgumentTable[idx] / 4;
        uint32_t *args = (uint32_t *)(uintptr_t)(c->regs[ESP] + 4); /* first arg (see i386 dispatcher) */
        if (trace()) fprintf( stderr, "wasm_x86: syscall %04x (%d args) -> handler %p\n", num, nargs, fn );
        if (num == 0x2c) /* NtTerminateProcess(handle, exit_status) */
            fprintf( stderr, "wasm_x86: *** NtTerminateProcess handle=%08x exit_status=%08x (%u) *** [%llu guest insns]\n",
                     args[0], args[1], args[1], (unsigned long long)g_total_insns );
        if (num == 0x43) /* NtContinue(context, alert): restore CPU state and jump */
        {
            uint32_t ctxp = args[0];
            c->regs[EDI]=rd32(ctxp+0x9c); c->regs[ESI]=rd32(ctxp+0xa0); c->regs[EBX]=rd32(ctxp+0xa4);
            c->regs[EDX]=rd32(ctxp+0xa8); c->regs[ECX]=rd32(ctxp+0xac); c->regs[EAX]=rd32(ctxp+0xb0);
            c->regs[EBP]=rd32(ctxp+0xb4); c->eflags=rd32(ctxp+0xc0); c->regs[ESP]=rd32(ctxp+0xc4);
            c->eip=rd32(ctxp+0xb8); c->lf_size=0;
            if (trace()) fprintf( stderr, "wasm_x86: NtContinue -> eip=%08x esp=%08x eax=%08x ebx=%08x\n",
                                  c->eip, c->regs[ESP], c->regs[EAX], c->regs[EBX] );
            return 1;
        }
        if (syscall_returns_void( num )) { call_handler_void( fn, nargs, args ); c->regs[EAX] = 0; }
        else c->regs[EAX] = call_handler( fn, nargs, args );
        if (table == 1) { extern void wasm_vm_sync_shared(void); wasm_vm_sync_shared(); } /* win32u wrote shared session mem */
        /* The handler (e.g. NtRaiseException) may have set up an exception frame
         * and redirected eip to KiUserExceptionDispatcher; don't clobber it. */
        if (g_wasm_exc_pending) { g_wasm_exc_pending = 0; return 1; }
        if (trace()) fprintf( stderr, "wasm_x86: syscall %04x returned eax=%08x ret_eip=%08x\n", num, c->regs[EAX], ret_eip );
        c->eip = ret_eip;
        return 1;
    }
    if (target == addr_unixcall)
    {
        /* i386 __wine_unix_call_dispatcher stack on entry (after the call):
         * [esp]=ret_eip, then the arguments of
         *   __wine_unix_call( unixlib_handle_t handle, unsigned int code, void *args )
         * - and the handle is a UINT64, so it occupies TWO slots.  The real
         * dispatcher (signal_i386.c) reads them at (%esp), 8(%esp) and 12(%esp)
         * once the return address is popped; our pointers fit in the low half.
         *
         * Getting this wrong is quiet rather than loud: `code` lands on the
         * handle's high half, which is always 0, so every unix call runs entry 0
         * (process_attach) and returns STATUS_SUCCESS.  opengl32 then "loaded"
         * and every get_pixel_formats came back with zero formats, which SDL
         * reports as "No matching GL pixel format available". */
        uint32_t ret_eip = pop32( c );
        unixlib_handle_t handle = rd32( c->regs[ESP] + 0 );
        unsigned int code = rd32( c->regs[ESP] + 8 );
        void *args = (void *)(uintptr_t)rd32( c->regs[ESP] + 12 );
        const unixlib_entry_t *funcs = (const unixlib_entry_t *)(uintptr_t)handle;
        /* Capture opengl32's unixlib funcs array the first time a GL call comes
         * through - identified by the caller (ret_eip) sitting inside opengl32.
         * The GL-thunk bypass then calls funcs[code] directly. */
        if (!g_ogl_handle && g_ogl_base && ret_eip >= g_ogl_base && ret_eip < g_ogl_base + g_ogl_size)
            g_ogl_handle = (uintptr_t)handle;
        if (trace()) fprintf( stderr, "wasm_x86: unix_call handle=%llx code=%u args=%p\n",
                              (unsigned long long)handle, code, args );
        if (!handle) {
            /* A PE DLL whose unix companion was never loaded (handle stayed 0).
             * Calling funcs[code] off a NULL table traps; return an error status
             * so the caller can fall back instead of crashing the process. */
            static int warned; if (!warned++) fprintf( stderr, "wasm_x86: unix_call with NULL handle (ret_eip=%08x code=%u) -> STATUS_NOT_IMPLEMENTED\n", ret_eip, code );
            c->regs[EAX] = 0xC0000002 /* STATUS_NOT_IMPLEMENTED */;
            c->eip = ret_eip;
            return 1;
        }
        { static int diag = -1;
          if (diag == -1) diag = getenv( "WASM_DIAG" ) ? 1 : 0;
          if (diag)
          {   /* mark "inside code", then "done with code" - the page can tell
               * a call that is still running from one that returned. */
              extern void webwine_diag( int a, int b );
              webwine_diag( (int)code, (int)ret_eip );
              c->regs[EAX] = funcs[code]( args );
              webwine_diag( (int)code | 0x40000000, (int)ret_eip );
              c->eip = ret_eip;
              return 1;
          } }
        c->regs[EAX] = funcs[code]( args );
        /* WASM_UCALL=1: report each unix call that answers STATUS_NOT_IMPLEMENTED
         * once.  For opengl32 that is exactly the set of GL entry points the
         * driver could not resolve and the game is calling anyway - the quiet
         * way a GLES driver loses a fixed-function matrix op. */
        if (c->regs[EAX] == 0xC0000002u)
        {
            static int on = -1;
            static unsigned char seen[4096];
            if (on == -1) on = getenv( "WASM_UCALL" ) ? 1 : 0;
            if (on && code < 4096 && !seen[code])
            { seen[code] = 1; fprintf( stderr, "wasm_x86: unix call %u not implemented\n", code ); }
        }
        c->eip = ret_eip;
        return 1;
    }
    fprintf( stderr, "wasm_x86: guest transferred to UNKNOWN native addr %08x (esp=%08x)\n",
             target, c->regs[ESP] );
    c->running = 0;
    return 0;
}

/* ---- thread entry: mirror signal_i386.c init_syscall_frame ---- */

void DECLSPEC_NORETURN signal_start_thread( PRTL_THREAD_START_ROUTINE entry, void *arg, TEB *teb )
{
    CONTEXT context = { 0 };
    CONTEXT *ctx;
    uint32_t *stack;

    addr_syscall  = (uint32_t)(uintptr_t)__wine_syscall_dispatcher;
    addr_unixcall = (uint32_t)(uintptr_t)__wine_unix_call_dispatcher;
    addr_apc      = (uint32_t)(uintptr_t)pKiUserApcDispatcher;
    addr_exc      = (uint32_t)(uintptr_t)pKiUserExceptionDispatcher;
    addr_cb       = (uint32_t)(uintptr_t)pKiUserCallbackDispatcher;
    (void)addr_apc; (void)addr_exc; (void)addr_cb;

    /* TEB->WOW32Reserved (offset 0xc0) is the syscall-dispatcher pointer that
     * half the i386 syscall thunks call through (`call fs:[0xc0]`). Native wine
     * sets this in signal_i386.c, which we don't compile; set it ourselves. */
    *(uint32_t *)((char *)teb + 0xc0) = addr_syscall;

    context.ContextFlags = CONTEXT_ALL;
    context.EFlags = 0x202;
    context.Eax = (DWORD)(ULONG_PTR)entry;
    context.Ebx = (DWORD)(ULONG_PTR)arg;
    context.Esp = (DWORD)(ULONG_PTR)teb->Tib.StackBase - 16;
    context.Eip = (DWORD)(ULONG_PTR)pRtlUserThreadStart;

    /* place the CONTEXT on the guest stack; LdrInitializeThunk receives &ctx */
    ctx = (CONTEXT *)((ULONG_PTR)context.Esp & ~3) - 1;
    *ctx = context;

    stack = (uint32_t *)ctx;
    *(--stack) = 0;
    *(--stack) = 0;
    *(--stack) = 0;
    *(--stack) = (uint32_t)(ULONG_PTR)ctx;
    *(--stack) = 0xdeadbabe;

    g_cpu.regs[ESP] = (uint32_t)(ULONG_PTR)stack;
    g_cpu.eip = (uint32_t)(ULONG_PTR)pLdrInitializeThunk;
    g_cpu.eflags = 0x202;
    g_cpu.lf_size = 0;
    g_cpu.fs_base = (uint32_t)(uintptr_t)teb;   /* i386 fs -> TEB linear address */
    g_cpu.gs_base = 0;
    g_cpu.fpcw = 0x037f; g_cpu.fpsw = 0; g_cpu.fptop = 0;  /* x87 default state */

    if (trace())
        fprintf( stderr, "wasm_x86: signal_start_thread entry=%p thunk=%p teb=%p\n",
                 (void*)entry, pLdrInitializeThunk, (void*)teb );

    run( &g_cpu );

    fprintf( stderr, "wasm_x86: initial thread run returned (eip=%08x) — exiting\n", g_cpu.eip );
    for (;;) exit( g_cpu.exit_code );
}
