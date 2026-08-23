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
static inline uint8_t  rd8 ( uint32_t a ){ return *(uint8_t  *)(uintptr_t)a; }
static inline uint16_t rd16( uint32_t a ){ return *(uint16_t *)(uintptr_t)a; }
static inline uint32_t rd32( uint32_t a ){ return *(uint32_t *)(uintptr_t)a; }
static inline void wr8 ( uint32_t a, uint8_t  v ){ *(uint8_t  *)(uintptr_t)a = v; }
static inline void wr16( uint32_t a, uint16_t v ){ *(uint16_t *)(uintptr_t)a = v; }
static inline void wr32( uint32_t a, uint32_t v ){ *(uint32_t *)(uintptr_t)a = v; }

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
       NAT_SDL_LOCK, NAT_SDL_UNLOCK, NAT_SDL_CLOSE, NAT_VLINE, NAT_SURFBLIT,
       NAT_SDL_POLL, NAT_SDL_RELMOUSE, NAT_MVLINE, NAT_SURFSPAN, NAT_LIBDIV, NAT_MHLINE, NAT_GLSTATE, NAT_GLSAMPLER, NAT_VLINE1, NAT_CRC32 };
static uint64_t g_count_hits;   /* diagnostic: executions of a watched address */
static uint64_t g_vl_calls, g_vl_iters;   /* native vlineasm4: entries and loop iterations */
static uint64_t g_sb_calls, g_sb_iters;   /* native surface blit: entries and iterations */
static uint64_t g_mv_calls, g_mv_iters;   /* native mvlineasm4: entries and loop iterations */
#define NAT_SLOTS 256
static uint32_t g_nat_addr[NAT_SLOTS];
static uint8_t  g_nat_kind[NAT_SLOTS];
static int g_nat_ready = 0;
#define NAT_SLOT(a) (((a) >> 4) & (NAT_SLOTS - 1))

static void nat_register( uint32_t addr, int kind, const char *what )
{
    if (!addr) return;
    unsigned s = NAT_SLOT( addr );
    if (g_nat_addr[s]) { fprintf( stderr, "wasm_x86: native %s: slot busy, skipped\n", what ); return; }
    g_nat_addr[s] = addr; g_nat_kind[s] = (uint8_t)kind;
    fprintf( stderr, "wasm_x86: native %s @ %08x\n", what, addr );
}

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

static void nat_init( struct x86cpu *c )
{
    /* msvcrt is loaded well after the exe base is known, so keep retrying on
     * later ticks rather than giving up on the first look. */
    uint32_t base = find_module( c, "msvcrt.dll" );
    if (!base) return;
    g_nat_ready = 1;
    nat_register( pe_export( base, "memmove" ), NAT_MEMMOVE, "memmove" );
    nat_register( pe_export( base, "memcpy" ),  NAT_MEMMOVE, "memcpy" );
    nat_register( pe_export( base, "memset" ),  NAT_MEMSET,  "memset" );
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

    c->regs[EAX] = eax; c->regs[EBX] = ebx; c->regs[ECX] = ecx;
    c->regs[EDX] = edx; c->regs[ESI] = esi; c->regs[EDI] = edi; c->regs[EBP] = ebp;
    set_lazy( c, K_ADD, prev_edi, fbstep, edi, 4 );      /* the add that ended it */
    c->eip = b + ND_VLINE_LEN;
    return 1;
}

static void nat_arm_vline( void )
{
    uint32_t va = ND_VLINE + (uint32_t)nd_slide;
    if (nd_vline_skeleton_ok( va )) nat_register( va, NAT_VLINE, "vlineasm4" );
    else fprintf( stderr, "wasm_x86: vlineasm4 skeleton differs at %08x - left interpreted\n", va );
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

static void nat_arm_mvline( void )
{
    uint32_t va = ND_MVLINE + (uint32_t)nd_slide, cb = 0;
    if (nd_mvline_skeleton_ok( va, &cb )) nat_register( va, NAT_MVLINE, "mvlineasm4" );
    else fprintf( stderr, "wasm_x86: mvlineasm4 skeleton differs at %08x - left interpreted\n", va );
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
#define SDL_EV_MOUSEMOTION 1024u

/* Filled by the win32u ring drain (dlls/win32u/message.c) from page events. */
int g_mouse_dx, g_mouse_dy;      /* accumulated pointer-lock motion */
int g_mouse_buttons;             /* SDL button mask */
int g_mouse_x = 160, g_mouse_y = 100;

static int sdl_poll_event( struct x86cpu *c )
{
    static uint64_t last_synth_flip;
    uint32_t ev = garg( c, 0 );
    int dx = g_mouse_dx, dy = g_mouse_dy;

    if ((!dx && !dy) || !ev) return 0;      /* nothing pending: run the real one */

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

static int nat_libdivide( struct x86cpu *c )
{
    uint32_t esp  = c->regs[ESP];
    uint32_t sret = c->regs[EAX];
    uint32_t bf, h;
    uint64_t key;
    int w;

    if (g_ld_filling) return 0;                  /* the nested run re-enters here */
    bf  = rd32( esp + 4 );                       /* branchfree: the one stack arg */
    key = ((((uint64_t)c->regs[ECX] << 32) | c->regs[EDX]) << 1) | (bf != 0);
    h   = (uint32_t)((key * 0x9e3779b97f4a7c15ull) >> 52) & (LD_SETS - 1);

    g_ld_calls++;
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

    if (tab < base || tab >= base + ND_GLSTATE_LEN) return 0;      /* not a GL table */
    if (rd32( ND_GLPROC_PROBE + (uint32_t)nd_slide )) return 0;    /* a real GL: run it */
    c->regs[EAX] = 0;                                /* "not found" */
    c->eip = rd32( esp );
    c->regs[ESP] = esp + 4;
    return 1;
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
    uint32_t sh  = rd8 ( b + 0x04 ) & 31;
    uint32_t bpl = rd32( b + 0x07 );
    uint32_t eax = c->regs[EAX], ebx = c->regs[EBX], ecx = c->regs[ECX];
    uint32_t edx = c->regs[EDX], esi = c->regs[ESI], edi = c->regs[EDI], ebp = c->regs[EBP];
    unsigned n = 0;

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

    c->regs[EAX] = eax; c->regs[EBX] = ebx; c->regs[ECX] = ecx;
    c->regs[EDX] = edx; c->regs[ESI] = esi; c->regs[EDI] = edi; c->regs[EBP] = ebp;
    c->eip = b + ND_VLINE1_LEN;          /* the pop %ebp after the loop */
    return 1;
}

static void nat_arm_vline1( void )
{
    uint32_t b = ND_VLINE1 + (uint32_t)nd_slide;
    unsigned i;
    for (i = 0; i < ND_VLINE1_LEN; i++)
        if (i == 0x04 || (i >= 0x07 && i <= 0x0a)) continue;   /* the patched fields */
        else if (rd8( b + i ) != nd_vline1_code[i])
        { fprintf( stderr, "wasm_x86: vlineasm1 skeleton differs at %08x - left interpreted\n", b ); return; }
    nat_register( b, NAT_VLINE1, "vlineasm1" );
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

static int nat_call( struct x86cpu *c, int kind )
{
    if (kind == NAT_CRC32)   return nat_crc32( c );
    if (kind == NAT_AGELOOP) return nat_ageloop( c );
    if (kind == NAT_VLINE)   return nat_vlineasm4( c );
    if (kind == NAT_MVLINE)  return nat_mvlineasm4( c );
    if (kind == NAT_MHLINE)  return nat_mhlineskip( c );
    if (kind == NAT_VLINE1)  return nat_vlineasm1( c );
    if (kind == NAT_GLSTATE) return nat_inthash_find( c );
    if (kind == NAT_GLSAMPLER) return nat_glsampler( c );
    if (kind == NAT_LIBDIV)   return nat_libdivide( c );
    if (kind == NAT_SURFSPAN) return nat_surfspan( c );
    if (kind == NAT_SURFBLIT) return nat_surfblit( c );
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
    /* Say yes to relative mode: the game then uses xrel/yrel, which we supply. */
    case NAT_SDL_RELMOUSE: gret( c, 0 ); return 1;
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

static int cond( struct x86cpu *c, int cc )
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
                          g_flip_addr = ND_VIDEONEXTPAGE + (uint32_t)nd_slide;
                          nat_register( g_flip_addr, NAT_FLIP, "frame flip" );
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
                          if (!getenv( "WASM_NO_AUDIO" )) nat_arm_audio();
                          if (!getenv( "WASM_NO_VLINE" )) nat_arm_vline();
                          if (!getenv( "WASM_NO_MVLINE" )) nat_arm_mvline();
                          if (!getenv( "WASM_NO_MHLINE" )) nat_arm_mhline();
                          if (!getenv( "WASM_NO_VLINE1" )) nat_arm_vline1();
                          if (!getenv( "WASM_NO_CRC32" )) nat_arm_crc32();
                          if (!getenv( "WASM_NO_GLSTUB" )) { nat_arm_inthash(); nat_arm_glsampler(); }
                          if (!getenv( "WASM_NO_SURFBLIT" )) nat_arm_surfblit();
                          g_skip_blit = getenv( "WASM_KEEP_BLIT" ) ? 0 : 1;
                          if (!getenv( "WASM_NO_SURFSPAN" )) nat_arm_surfspan();
                          g_ld_verify = getenv( "WASM_LIBDIV_VERIFY" ) ? 1 : 0;
                          if (!getenv( "WASM_NO_LIBDIV" )) nat_arm_libdivide();
                          if (!getenv( "WASM_NO_MOUSE" )) nat_arm_mouse();
                          fprintf( stderr, "wasm_x86: exe base=%08x slide=%d\n", ib, nd_slide ); }
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
              if (!g_nat_ready && g_slide_ok && (nat_tries++ & 0xff) == 0 && nat_tries < 400000) nat_init( c ); }
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
            static uint64_t tp_last_insns = 0, tp_last_flip = 0, tp_last_hits = 0, tp_last_audio = 0, tp_last_calls = 0, tp_last_vlc = 0, tp_last_vli = 0, tp_last_sbc = 0, tp_last_sbi = 0, tp_last_mvc = 0, tp_last_mvi = 0, tp_last_mhc = 0, tp_last_mhi = 0;
            if (tp_on == -1) { tp_on = getenv( "WASM_TPUT" ) ? 1 : 0; g_histo_on = getenv( "WASM_HISTO" ) ? 1 : 0;
                               g_prof_on = getenv( "WASM_PROF" ) ? 1 : 0; }
            if (tp_on)
            {
                double now = emscripten_get_now();
                if (tp_start == 0) { tp_start = tp_last = now; tp_last_insns = g_total_insns; tp_last_flip = g_flip_count; }
                else if (now - tp_last >= 1000.0)   /* sample once per wall-second */
                {
                    double dt = (now - tp_last) / 1000.0;
                    double fps = (double)(g_flip_count - tp_last_flip) / dt;
                    double mips = (double)(g_total_insns - tp_last_insns) / dt / 1e6;
                    fprintf( stderr, "FPSSAMPLE t=%.1f flips=%llu fps=%.1f mips=%.1f loop/frame=%.0f audio=%.1fM/s calls=%llu vl=%.0f/%.0f mv=%.0f/%.0f mh=%.0f/%.0f sb=%.0f/%.0f ld=%llu/%llu ldchk=%llu/%llu kinsn/frame=%.0f\n",
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
                             fps > 0 ? (double)(g_sb_calls - tp_last_sbc) / fps : 0.0,
                             fps > 0 ? (double)(g_sb_iters - tp_last_sbi) / fps : 0.0,
                             (unsigned long long)g_ld_hits, (unsigned long long)g_ld_calls,
                             (unsigned long long)g_ld_ok, (unsigned long long)g_ld_bad,
                             fps > 0 ? (double)(g_total_insns - tp_last_insns) / fps / 1000.0 : 0.0 );
                    tp_last_sbc = g_sb_calls; tp_last_sbi = g_sb_iters;
                    tp_last_mvc = g_mv_calls; tp_last_mvi = g_mv_iters;
                    tp_last_mhc = g_mh_calls; tp_last_mhi = g_mh_iters;
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
        last_eip = start;
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

static uint32_t addr_syscall, addr_unixcall, addr_apc, addr_exc, addr_cb;

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
