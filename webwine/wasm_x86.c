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

static int cond( struct x86cpu *c, int cc )
{
    uint32_t f = get_flags( c );
    int res;
    switch (cc >> 1)
    {
    case 0: res = f & OF; break;                          /* O */
    case 1: res = f & CF; break;                          /* B/C */
    case 2: res = f & ZF; break;                          /* Z/E */
    case 3: res = (f & CF) || (f & ZF); break;            /* BE */
    case 4: res = f & SF; break;                          /* S */
    case 5: res = f & PF; break;                          /* P */
    case 6: res = !!(f & SF) != !!(f & OF); break;        /* L */
    case 7: res = (!!(f & SF) != !!(f & OF)) || (f & ZF); break; /* LE */
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

/* Execute until the guest transfers to a native dispatcher (returns 1, target
 * left in c->eip) or stops. */
static void run( struct x86cpu *c )
{
    uint32_t last_eip = c->eip;
    c->running = 1;
    while (c->running)
    {
        g_total_insns++;
        /* a native trampoline address (small wasm table index) -> back to C */
        if (c->eip < 0x10000)
        {
            if (c->eip == 0)
                fprintf( stderr, "wasm_x86: transfer to NULL from insn @ %08x "
                         "(esp=%08x [esp]=%08x [esp+4]=%08x eax=%08x ebx=%08x ecx=%08x edx=%08x)\n",
                         last_eip, c->regs[ESP], rd32(c->regs[ESP]), rd32(c->regs[ESP]+4),
                         c->regs[EAX], c->regs[EBX], c->regs[ECX], c->regs[EDX] );
            if (!wasm_x86_dispatch( c, c->eip )) return;
            continue;
        }
        uint32_t start = c->eip;
        last_eip = start;
        struct decode d = { 4, 4, 0, 0, c->eip };
        uint8_t op;

        /* prefixes */
        for (;;)
        {
            op = f8( &d );
            if (op == 0x66) { d.opsize = 2; continue; }
            if (op == 0x67) { d.addrsize = 2; continue; }
            if (op == 0xf2 || op == 0xf3) { d.rep = op; continue; }
            if (op == 0x64) { d.seg = (int)c->fs_base; continue; }  /* fs -> TEB */
            if (op == 0x65) { d.seg = (int)c->gs_base; continue; }  /* gs */
            if (op == 0x26 || op == 0x2e || op == 0x36 || op == 0x3e) { d.seg = 0; continue; } /* flat */
            if (op == 0xf0) continue; /* lock */
            break;
        }

        int os = d.opsize;
        struct modrm m;
        uint32_t a, b, r;

        switch (op)
        {
        case 0x88: m = decode_modrm(c,&d); write_rm(c,&m,1, read_reg(c,m.reg,1)); break;
        case 0x89: m = decode_modrm(c,&d); write_rm(c,&m,os, read_reg(c,m.reg,os)); break;
        case 0x8a: m = decode_modrm(c,&d); write_reg(c,m.reg,1, read_rm(c,&m,1)); break;
        case 0x8b: m = decode_modrm(c,&d); write_reg(c,m.reg,os, read_rm(c,&m,os)); break;
        case 0x8d: m = decode_modrm(c,&d); write_reg(c,m.reg,os, m.ea); break; /* lea */
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
            default: unimplemented(c,start,op); return;
            }
            break;
        case 0x8f: m = decode_modrm(c,&d); write_rm(c,&m,os, pop32(c)); break;
        case 0x9c: push32(c, get_flags(c)); break;           /* pushfd */
        case 0x9d: c->eflags = pop32(c); c->lf_size = 0; break; /* popfd */

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
            case 2: { int ci=(get_flags(c)&CF)?1:0; r=a+b+ci; write_rm(c,&m,sz,r); set_lazy(c,K_ADC,a,b,r,sz); c->lf_cin=ci; } break; /* adc */
            case 3: { int ci=(get_flags(c)&CF)?1:0; r=a-b-ci; write_rm(c,&m,sz,r); set_lazy(c,K_SBB,a,b,r,sz); c->lf_cin=ci; } break; /* sbb */
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
        case 0x10: { int ci=(get_flags(c)&CF)?1:0; m=decode_modrm(c,&d); a=read_rm(c,&m,1); b=read_reg(c,m.reg,1); r=a+b+ci; write_rm(c,&m,1,r); set_lazy(c,K_ADC,a,b,r,1); c->lf_cin=ci; } break;
        case 0x11: { int ci=(get_flags(c)&CF)?1:0; m=decode_modrm(c,&d); a=read_rm(c,&m,os); b=read_reg(c,m.reg,os); r=a+b+ci; write_rm(c,&m,os,r); set_lazy(c,K_ADC,a,b,r,os); c->lf_cin=ci; } break;
        case 0x13: { int ci=(get_flags(c)&CF)?1:0; m=decode_modrm(c,&d); a=read_reg(c,m.reg,os); b=read_rm(c,&m,os); r=a+b+ci; write_reg(c,m.reg,os,r); set_lazy(c,K_ADC,a,b,r,os); c->lf_cin=ci; } break;
        case 0x18: { int ci=(get_flags(c)&CF)?1:0; m=decode_modrm(c,&d); a=read_rm(c,&m,1); b=read_reg(c,m.reg,1); r=a-b-ci; write_rm(c,&m,1,r); set_lazy(c,K_SBB,a,b,r,1); c->lf_cin=ci; } break;
        case 0x19: { int ci=(get_flags(c)&CF)?1:0; m=decode_modrm(c,&d); a=read_rm(c,&m,os); b=read_reg(c,m.reg,os); r=a-b-ci; write_rm(c,&m,os,r); set_lazy(c,K_SBB,a,b,r,os); c->lf_cin=ci; } break;
        case 0x1b: { int ci=(get_flags(c)&CF)?1:0; m=decode_modrm(c,&d); a=read_reg(c,m.reg,os); b=read_rm(c,&m,os); r=a-b-ci; write_reg(c,m.reg,os,r); set_lazy(c,K_SBB,a,b,r,os); c->lf_cin=ci; } break;

        /* pushad / popad */
        case 0x60: { uint32_t sp=c->regs[ESP]; push32(c,c->regs[EAX]); push32(c,c->regs[ECX]); push32(c,c->regs[EDX]); push32(c,c->regs[EBX]); push32(c,sp); push32(c,c->regs[EBP]); push32(c,c->regs[ESI]); push32(c,c->regs[EDI]); } break;
        case 0x61: c->regs[EDI]=pop32(c); c->regs[ESI]=pop32(c); c->regs[EBP]=pop32(c); pop32(c); c->regs[EBX]=pop32(c); c->regs[EDX]=pop32(c); c->regs[ECX]=pop32(c); c->regs[EAX]=pop32(c); break;

        /* imul r, r/m, imm */
        case 0x69: m=decode_modrm(c,&d); a=read_rm(c,&m,os); b=(os==2?f16(&d):f32(&d)); write_reg(c,m.reg,os,(int32_t)a*(int32_t)b); break;
        case 0x6b: m=decode_modrm(c,&d); a=read_rm(c,&m,os); b=(int32_t)(int8_t)f8(&d); write_reg(c,m.reg,os,(int32_t)a*(int32_t)b); break;

        /* shift/rotate group: C0/C1 (imm8), D0/D1 (by 1), D2/D3 (by cl) */
        case 0xc0: case 0xc1: case 0xd0: case 0xd1: case 0xd2: case 0xd3:
        {
            int sz = (op&1)?os:1;
            m=decode_modrm(c,&d);
            uint32_t cnt;
            if (op==0xc0||op==0xc1) cnt=f8(&d);
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

        /* grp3: F6/F7 test/not/neg/mul/imul/div/idiv */
        case 0xf6: case 0xf7:
        {
            int sz=(op&1)?os:1;
            m=decode_modrm(c,&d);
            a=read_rm(c,&m,sz);
            switch (m.reg)
            {
            case 0: case 1: b=(sz==1)?f8(&d):(os==2?f16(&d):f32(&d)); set_lazy(c,K_LOGIC,a,b,a&b,sz); break; /* test */
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
            int sz=(op&1)?os:1; uint32_t cnt=(d.rep)?c->regs[ECX]:1; int delta=(get_flags(c)&DF)?-sz:sz;
            while (cnt--) { uint32_t v=(sz==1)?rd8(c->regs[ESI]):sz==2?rd16(c->regs[ESI]):rd32(c->regs[ESI]);
                            if(sz==1)wr8(c->regs[EDI],v); else if(sz==2)wr16(c->regs[EDI],v); else wr32(c->regs[EDI],v);
                            c->regs[ESI]+=delta; c->regs[EDI]+=delta; }
            if (d.rep) c->regs[ECX]=0;
            break;
        }
        case 0xaa: case 0xab: /* stos */
        {
            int sz=(op&1)?os:1; uint32_t cnt=(d.rep)?c->regs[ECX]:1; int delta=(get_flags(c)&DF)?-sz:sz; uint32_t v=c->regs[EAX];
            while (cnt--) { if(sz==1)wr8(c->regs[EDI],v); else if(sz==2)wr16(c->regs[EDI],v); else wr32(c->regs[EDI],v); c->regs[EDI]+=delta; }
            if (d.rep) c->regs[ECX]=0;
            break;
        }
        case 0xac: case 0xad: /* lods */
        {
            int sz=(op&1)?os:1; int delta=(get_flags(c)&DF)?-sz:sz;
            uint32_t v=(sz==1)?rd8(c->regs[ESI]):sz==2?rd16(c->regs[ESI]):rd32(c->regs[ESI]);
            write_reg(c,EAX,sz,v); c->regs[ESI]+=delta;
            break;
        }
        case 0xf5: c->eflags ^= CF; break;  /* cmc */
        case 0xf8: c->eflags &= ~CF; break;  /* clc */
        case 0xf9: c->eflags |= CF; break;   /* stc */
        case 0xfc: c->eflags &= ~DF; break;  /* cld */
        case 0xfd: c->eflags |= DF; break;   /* std */

        /* ---- x87 FPU (0xd8-0xdf) ---- */
        case 0xd8: /* arithmetic with m32real / st(i), dest st0 */
        {
            m = decode_modrm(c,&d);
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
            m = decode_modrm(c,&d);
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
            m = decode_modrm(c,&d);
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
            m = decode_modrm(c,&d);
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
            m = decode_modrm(c,&d);
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
            m = decode_modrm(c,&d);
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
            m = decode_modrm(c,&d);
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
            m = decode_modrm(c,&d);
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

        case 0x0f: /* two-byte */
        {
            uint8_t op2 = f8(&d);
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
            case 0xbc: /* bsf */ m=decode_modrm(c,&d); a=read_rm(c,&m,os); if(a){ int i=0; while(!((a>>i)&1))i++; write_reg(c,m.reg,os,i); c->eflags&=~ZF;} else c->eflags|=ZF; c->lf_size=0; break;
            case 0xbd: /* bsr */ m=decode_modrm(c,&d); a=read_rm(c,&m,os); if(a){ int i=os*8-1; while(!((a>>i)&1))i--; write_reg(c,m.reg,os,i); c->eflags&=~ZF;} else c->eflags|=ZF; c->lf_size=0; break;
            case 0xa4: case 0xa5: /* SHLD r/m, r, imm8/cl */
            case 0xac: case 0xad: /* SHRD r/m, r, imm8/cl */
            {
                int left = (op2==0xa4||op2==0xa5);
                m=decode_modrm(c,&d);
                uint32_t dst=read_rm(c,&m,os), src=read_reg(c,m.reg,os);
                uint32_t cnt = (op2==0xa4||op2==0xac) ? f8(&d) : (c->regs[ECX]&0xff);
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
            case 0x1f: decode_modrm(c,&d); break; /* nop r/m */
            case 0x31: /* rdtsc */ c->regs[EAX]=0; c->regs[EDX]=0; break;
            case 0xa2: /* cpuid */ c->regs[EAX]=0; c->regs[EBX]=0; c->regs[ECX]=0; c->regs[EDX]=0; break;

            /* ---- SSE/SSE2 (enough for memset/memcpy/strlen in ntdll) ---- */
            case 0x10: case 0x28: /* movups/movaps/movss/movsd xmm, xmm/m */
                m=decode_modrm(c,&d);
                if (m.is_reg) memcpy(c->xmm[m.reg], c->xmm[m.rm], 16);
                else memcpy(c->xmm[m.reg], (void*)(uintptr_t)m.ea, (d.rep==0xf3)?4:(d.rep==0xf2)?8:16);
                break;
            case 0x11: case 0x29: /* store */
                m=decode_modrm(c,&d);
                if (m.is_reg) memcpy(c->xmm[m.rm], c->xmm[m.reg], 16);
                else memcpy((void*)(uintptr_t)m.ea, c->xmm[m.reg], (d.rep==0xf3)?4:(d.rep==0xf2)?8:16);
                break;
            case 0x6f: /* movdqa/movdqu load */
                m=decode_modrm(c,&d);
                if (m.is_reg) memcpy(c->xmm[m.reg], c->xmm[m.rm], 16);
                else memcpy(c->xmm[m.reg], (void*)(uintptr_t)m.ea, 16);
                break;
            case 0x7f: /* movdqa/movdqu store */
                m=decode_modrm(c,&d);
                if (m.is_reg) memcpy(c->xmm[m.rm], c->xmm[m.reg], 16);
                else memcpy((void*)(uintptr_t)m.ea, c->xmm[m.reg], 16);
                break;
            case 0x6e: /* movd xmm, r/m32 */
                m=decode_modrm(c,&d); memset(c->xmm[m.reg],0,16);
                { uint32_t v=read_rm(c,&m,4); memcpy(c->xmm[m.reg],&v,4); } break;
            case 0x7e: /* f3: movq xmm,xmm/m64 ; else movd r/m32,xmm */
                m=decode_modrm(c,&d);
                if (d.rep==0xf3) { memset(c->xmm[m.reg],0,16);
                    if (m.is_reg) memcpy(c->xmm[m.reg], c->xmm[m.rm], 8); else memcpy(c->xmm[m.reg],(void*)(uintptr_t)m.ea,8); }
                else { uint32_t v; memcpy(&v,c->xmm[m.reg],4); write_rm(c,&m,4,v); }
                break;
            case 0xd6: /* movq store */
                m=decode_modrm(c,&d);
                if (m.is_reg) { memset(c->xmm[m.rm],0,16); memcpy(c->xmm[m.rm], c->xmm[m.reg], 8); }
                else memcpy((void*)(uintptr_t)m.ea, c->xmm[m.reg], 8);
                break;
            case 0xef: /* pxor */
                m=decode_modrm(c,&d);
                { uint8_t *src = m.is_reg ? c->xmm[m.rm] : (uint8_t*)(uintptr_t)m.ea; int i; for(i=0;i<16;i++) c->xmm[m.reg][i]^=src[i]; }
                break;
            case 0x74: case 0x75: case 0x76: /* pcmpeqb/w/d */
                m=decode_modrm(c,&d);
                { uint8_t *src = m.is_reg ? c->xmm[m.rm] : (uint8_t*)(uintptr_t)m.ea; int i;
                  int step=(op2==0x74)?1:(op2==0x75)?2:4;
                  for(i=0;i<16;i+=step){ int eq=!memcmp(&c->xmm[m.reg][i],&src[i],step); memset(&c->xmm[m.reg][i],eq?0xff:0,step);} }
                break;
            case 0xd7: /* pmovmskb r32, xmm */
                m=decode_modrm(c,&d);
                { uint32_t mask=0; int i; for(i=0;i<16;i++) if(c->xmm[m.rm][i]&0x80) mask|=(1u<<i); write_reg(c,m.reg,4,mask); }
                break;
            case 0xfc: case 0xfd: case 0xfe: /* paddb/w/d */
                m=decode_modrm(c,&d);
                { uint8_t *src = m.is_reg ? c->xmm[m.rm] : (uint8_t*)(uintptr_t)m.ea; int i;
                  if(op2==0xfc) for(i=0;i<16;i++) c->xmm[m.reg][i]+=src[i];
                  else if(op2==0xfd){ uint16_t *a16=(uint16_t*)c->xmm[m.reg],*s16=(uint16_t*)src; for(i=0;i<8;i++) a16[i]+=s16[i]; }
                  else { uint32_t *a32=(uint32_t*)c->xmm[m.reg],*s32=(uint32_t*)src; for(i=0;i<4;i++) a32[i]+=s32[i]; } }
                break;
            case 0x12: case 0x13: case 0x16: case 0x17: /* movlps/movhps: treat as 8-byte move */
                m=decode_modrm(c,&d);
                if (op2==0x12||op2==0x16){ if(!m.is_reg) memcpy(c->xmm[m.reg]+((op2==0x16)?8:0),(void*)(uintptr_t)m.ea,8); }
                else { if(!m.is_reg) memcpy((void*)(uintptr_t)m.ea,c->xmm[m.reg]+((op2==0x17)?8:0),8); }
                break;
            case 0x57: /* xorps */
                m=decode_modrm(c,&d);
                { uint8_t *src = m.is_reg ? c->xmm[m.rm] : (uint8_t*)(uintptr_t)m.ea; int i; for(i=0;i<16;i++) c->xmm[m.reg][i]^=src[i]; }
                break;
            case 0x55: /* andnps */
                m=decode_modrm(c,&d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; int i; for(i=0;i<16;i++) c->xmm[m.reg][i]=(~c->xmm[m.reg][i])&s[i]; } break;
            /* --- SSE/SSE2 float arithmetic: prefix picks ps/pd/ss/sd --- */
            case 0x51: case 0x52: case 0x53: /* sqrt / rsqrt / rcp (unary on src) */
            case 0x58: case 0x59: case 0x5c: case 0x5d: case 0x5e: case 0x5f: /* add mul sub min div max */
            {
                m=decode_modrm(c,&d); uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint8_t *dst=c->xmm[m.reg];
                int dbl=(d.opsize==2||d.rep==0xf2), scal=(d.rep==0xf3||d.rep==0xf2);
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
                m=decode_modrm(c,&d); { int32_t v=(int32_t)read_rm(c,&m,4); uint8_t *dst=c->xmm[m.reg];
                    if(d.rep==0xf2){ double dv=v; memcpy(dst,&dv,8); } else { float fv=(float)v; memcpy(dst,&fv,4); } } break;
            case 0x2c: case 0x2d: /* cvt(t)ss2si / sd2si -> r32 */
                m=decode_modrm(c,&d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; double v;
                    if(d.rep==0xf2){ uint64_t b; memcpy(&b,s,8); memcpy(&v,&b,8); } else { float f; memcpy(&f,s,4); v=f; }
                    int32_t r=(op2==0x2c)?(int32_t)v:(int32_t)llrint(v); write_reg(c,m.reg,4,(uint32_t)r); } break;
            case 0x2e: case 0x2f: /* ucomiss/comiss (66 -> sd) -> EFLAGS */
                m=decode_modrm(c,&d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; double x,y;
                    if(d.opsize==2){ uint64_t xb,yb; memcpy(&xb,c->xmm[m.reg],8); memcpy(&yb,s,8); memcpy(&x,&xb,8); memcpy(&y,&yb,8); }
                    else { float xf,yf; memcpy(&xf,c->xmm[m.reg],4); memcpy(&yf,s,4); x=xf; y=yf; }
                    c->eflags &= ~(ZF|PF|CF|OF|SF|AF); c->lf_size=0;
                    if(isnan(x)||isnan(y)) c->eflags|=ZF|PF|CF; else if(x<y) c->eflags|=CF; else if(x==y) c->eflags|=ZF; }
                break;
            case 0x5a: /* cvtps2pd/pd2ps/ss2sd/sd2ss */
                m=decode_modrm(c,&d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint8_t *dst=c->xmm[m.reg];
                    if(d.rep==0xf3){ float f; memcpy(&f,s,4); double dv=f; memcpy(dst,&dv,8); }
                    else if(d.rep==0xf2){ uint64_t b; double dv; memcpy(&b,s,8); memcpy(&dv,&b,8); float f=(float)dv; memcpy(dst,&f,4); }
                    else if(d.opsize==2){ uint64_t b0,b1; double d0,d1; memcpy(&b0,s,8); memcpy(&b1,s+8,8); memcpy(&d0,&b0,8); memcpy(&d1,&b1,8); float f0=(float)d0,f1=(float)d1; uint8_t t[16]; memset(t,0,16); memcpy(t,&f0,4); memcpy(t+4,&f1,4); memcpy(dst,t,16); }
                    else { float f0,f1; memcpy(&f0,s,4); memcpy(&f1,s+4,4); double d0=f0,d1=f1; memcpy(dst,&d0,8); memcpy(dst+8,&d1,8); } }
                break;
            case 0x5b: /* cvtdq2ps / cvtps2dq(66) / cvttps2dq(f3) */
                m=decode_modrm(c,&d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint8_t *dst=c->xmm[m.reg]; uint8_t t[16]; int i;
                    if(d.rep==0xf3){ for(i=0;i<4;i++){ float f; memcpy(&f,s+i*4,4); int32_t v=(int32_t)f; memcpy(t+i*4,&v,4);} }
                    else if(d.opsize==2){ for(i=0;i<4;i++){ float f; memcpy(&f,s+i*4,4); int32_t v=(int32_t)llrintf(f); memcpy(t+i*4,&v,4);} }
                    else { for(i=0;i<4;i++){ int32_t v; memcpy(&v,s+i*4,4); float f=(float)v; memcpy(t+i*4,&f,4);} }
                    memcpy(dst,t,16); }
                break;
            case 0xe6: /* cvtdq2pd(f3) / cvttpd2dq(66) / cvtpd2dq(f2) */
                m=decode_modrm(c,&d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint8_t *dst=c->xmm[m.reg]; uint8_t t[16]; memset(t,0,16); int i;
                    if(d.rep==0xf3){ for(i=0;i<2;i++){ int32_t v; memcpy(&v,s+i*4,4); double dv=v; memcpy(t+i*8,&dv,8);} }
                    else { int trunc=(d.opsize==2); for(i=0;i<2;i++){ uint64_t b; double dv; memcpy(&b,s+i*8,8); memcpy(&dv,&b,8); int32_t v=trunc?(int32_t)dv:(int32_t)llrint(dv); memcpy(t+i*4,&v,4);} }
                    memcpy(dst,t,16); }
                break;
            case 0xc2: /* cmpps/pd/ss/sd imm8 predicate */
                m=decode_modrm(c,&d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint8_t *dst=c->xmm[m.reg]; uint8_t im=f8(&d);
                    int dbl=(d.opsize==2||d.rep==0xf2), scal=(d.rep==0xf3||d.rep==0xf2); int lanes=dbl?2:4, n=scal?1:lanes, i;
                    for(i=0;i<n;i++){ double x,y; if(dbl){ uint64_t xb,yb; memcpy(&xb,dst+i*8,8); memcpy(&yb,s+i*8,8); memcpy(&x,&xb,8); memcpy(&y,&yb,8);} else { float xf,yf; memcpy(&xf,dst+i*4,4); memcpy(&yf,s+i*4,4); x=xf; y=yf; }
                        int un=isnan(x)||isnan(y),res; switch(im&7){ case 0:res=(x==y);break; case 1:res=(x<y);break; case 2:res=(x<=y);break; case 3:res=un;break; case 4:res=!(x==y);break; case 5:res=!(x<y);break; case 6:res=!(x<=y);break; default:res=!un; }
                        if(dbl){ uint64_t mm=res?~0ULL:0; memcpy(dst+i*8,&mm,8);} else { uint32_t mm=res?~0U:0; memcpy(dst+i*4,&mm,4);} } }
                break;
            case 0xc6: /* shufps / shufpd(66) imm8 */
                m=decode_modrm(c,&d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint8_t *dst=c->xmm[m.reg]; uint8_t im=f8(&d); uint8_t t[16];
                    if(d.opsize==2){ memcpy(&t[0],&dst[(im&1)?8:0],8); memcpy(&t[8],&s[(im&2)?8:0],8); }
                    else { uint32_t *td=(uint32_t*)t,*dd=(uint32_t*)dst,*sd=(uint32_t*)s; td[0]=dd[(im>>0)&3]; td[1]=dd[(im>>2)&3]; td[2]=sd[(im>>4)&3]; td[3]=sd[(im>>6)&3]; }
                    memcpy(dst,t,16); }
                break;
            case 0xdb: /* pand */ m=decode_modrm(c,&d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; int i; for(i=0;i<16;i++) c->xmm[m.reg][i]&=s[i]; } break;
            case 0xdf: /* pandn */ m=decode_modrm(c,&d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; int i; for(i=0;i<16;i++) c->xmm[m.reg][i]=(~c->xmm[m.reg][i])&s[i]; } break;
            case 0xeb: /* por */ m=decode_modrm(c,&d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; int i; for(i=0;i<16;i++) c->xmm[m.reg][i]|=s[i]; } break;
            case 0x60: case 0x61: case 0x62: case 0x6c: /* punpckl bw/wd/dq/qdq */
            {
                m=decode_modrm(c,&d); uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint8_t tmp[16]; uint8_t *dst=c->xmm[m.reg];
                int u = op2==0x60?1:op2==0x61?2:op2==0x62?4:8; int i,o=0;
                for (i=0;i<8/ (u? u:1) + (u==8?1:0); i++) {} /* interleave low half */
                { int k; o=0; for(k=0;k<8;k+=u){ memcpy(&tmp[o],&dst[k],u); o+=u; memcpy(&tmp[o],&s[k],u); o+=u; if(o>=16)break;} }
                memcpy(dst,tmp,16); break;
            }
            case 0x68: case 0x69: case 0x6a: case 0x6d: /* punpckh */
            {
                m=decode_modrm(c,&d); uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint8_t tmp[16]; uint8_t *dst=c->xmm[m.reg];
                int u = op2==0x68?1:op2==0x69?2:op2==0x6a?4:8; int k,o=0;
                for(k=8;k<16;k+=u){ memcpy(&tmp[o],&dst[k],u); o+=u; memcpy(&tmp[o],&s[k],u); o+=u; if(o>=16)break;}
                memcpy(dst,tmp,16); break;
            }
            case 0x70: /* pshufd(66)/pshuflw(f2)/pshufhw(f3) */
            {
                m=decode_modrm(c,&d); uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint8_t im=f8(&d); uint8_t tmp[16];
                if (d.opsize==2) { uint32_t *sd=(uint32_t*)s,*td=(uint32_t*)tmp; int i; for(i=0;i<4;i++) td[i]=sd[(im>>(i*2))&3]; }
                else if (d.rep==0xf2) { memcpy(tmp,s,16); uint16_t *sw=(uint16_t*)s,*tw=(uint16_t*)tmp; int i; for(i=0;i<4;i++) tw[i]=sw[(im>>(i*2))&3]; }
                else { memcpy(tmp,s,16); uint16_t *sw=(uint16_t*)s,*tw=(uint16_t*)tmp; int i; for(i=0;i<4;i++) tw[4+i]=sw[4+((im>>(i*2))&3)]; }
                memcpy(c->xmm[m.reg],tmp,16); break;
            }
            case 0x71: case 0x72: case 0x73: /* psll/psrl/psra by imm8 (reg field selects op) */
            {
                m=decode_modrm(c,&d); uint8_t im=f8(&d); uint8_t *dst=c->xmm[m.rm];
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
                m=decode_modrm(c,&d); { uint8_t im=f8(&d); uint16_t *w=(uint16_t*)c->xmm[m.rm]; write_reg(c,m.reg,4,w[im&7]); } break;
            case 0xc4: /* pinsrw xmm, r/m16, imm8 */
                m=decode_modrm(c,&d); { uint32_t v=read_rm(c,&m,2); uint8_t im=f8(&d); uint16_t *w=(uint16_t*)c->xmm[m.reg]; w[im&7]=(uint16_t)v; } break;
            case 0x50: /* movmskps r32, xmm */
                m=decode_modrm(c,&d); { uint32_t mask=0; int i; for(i=0;i<4;i++) if(c->xmm[m.rm][i*4+3]&0x80) mask|=(1u<<i); write_reg(c,m.reg,4,mask); } break;
            case 0x64: case 0x65: case 0x66: /* pcmpgtb/w/d (signed) */
                m=decode_modrm(c,&d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint8_t *dst=c->xmm[m.reg]; int i;
                  if(op2==0x64){ for(i=0;i<16;i++) dst[i]=((int8_t)dst[i]>(int8_t)s[i])?0xff:0; }
                  else if(op2==0x65){ int16_t *a=(int16_t*)dst,*b=(int16_t*)s; for(i=0;i<8;i++) a[i]=(a[i]>b[i])?-1:0; }
                  else { int32_t *a=(int32_t*)dst,*b=(int32_t*)s; for(i=0;i<4;i++) a[i]=(a[i]>b[i])?-1:0; } } break;
            case 0xd1: case 0xd2: case 0xd3: /* psrlw/d/q by xmm */
            case 0xf1: case 0xf2: case 0xf3: /* psllw/d/q by xmm */
            case 0xe1: case 0xe2: /* psraw/psrad by xmm */
                m=decode_modrm(c,&d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint8_t *dst=c->xmm[m.reg];
                  uint64_t cnt; memcpy(&cnt,s,8);
                  int u=(op2==0xd1||op2==0xf1||op2==0xe1)?2:(op2==0xd3||op2==0xf3)?8:4;
                  int arith=(op2==0xe1||op2==0xe2), left=(op2==0xf1||op2==0xf2||op2==0xf3);
                  int n=16/u,i; for(i=0;i<n;i++){ uint64_t v=0; memcpy(&v,&dst[i*u],u); int bits=u*8;
                    if(cnt>=(uint64_t)bits){ if(arith){ int64_t sv=(int64_t)(v<<(64-bits)); sv>>=63; v=(uint64_t)sv; } else v=0; }
                    else if(left) v<<=cnt;
                    else if(arith){ int64_t sv=(int64_t)(v<<(64-bits)); sv>>=(64-bits); v=(uint64_t)(sv>>cnt); }
                    else v>>=cnt;
                    memcpy(&dst[i*u],&v,u);} } break;
            case 0xd4: /* paddq */ m=decode_modrm(c,&d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint64_t *a=(uint64_t*)c->xmm[m.reg],*b=(uint64_t*)s; a[0]+=b[0]; a[1]+=b[1]; } break;
            case 0xfb: /* psubq */ m=decode_modrm(c,&d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint64_t *a=(uint64_t*)c->xmm[m.reg],*b=(uint64_t*)s; a[0]-=b[0]; a[1]-=b[1]; } break;
            case 0xd5: /* pmullw */ m=decode_modrm(c,&d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint16_t *a=(uint16_t*)c->xmm[m.reg],*b=(uint16_t*)s; int i; for(i=0;i<8;i++) a[i]=(uint16_t)(a[i]*b[i]); } break;
            case 0xe5: /* pmulhw */ m=decode_modrm(c,&d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; int16_t *a=(int16_t*)c->xmm[m.reg],*b=(int16_t*)s; int i; for(i=0;i<8;i++) a[i]=(int16_t)(((int32_t)a[i]*b[i])>>16); } break;
            case 0xe4: /* pmulhuw */ m=decode_modrm(c,&d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint16_t *a=(uint16_t*)c->xmm[m.reg],*b=(uint16_t*)s; int i; for(i=0;i<8;i++) a[i]=(uint16_t)(((uint32_t)a[i]*b[i])>>16); } break;
            case 0xf4: /* pmuludq */ m=decode_modrm(c,&d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint32_t *a=(uint32_t*)c->xmm[m.reg],*b=(uint32_t*)s; uint64_t r0=(uint64_t)a[0]*b[0], r1=(uint64_t)a[2]*b[2]; uint64_t *o=(uint64_t*)c->xmm[m.reg]; o[0]=r0; o[1]=r1; } break;
            case 0xf5: /* pmaddwd */ m=decode_modrm(c,&d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; int16_t *a=(int16_t*)c->xmm[m.reg],*b=(int16_t*)s; int32_t r[4]; int i; for(i=0;i<4;i++) r[i]=(int32_t)a[i*2]*b[i*2]+(int32_t)a[i*2+1]*b[i*2+1]; memcpy(c->xmm[m.reg],r,16); } break;
            case 0xf6: /* psadbw */ m=decode_modrm(c,&d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint8_t *dst=c->xmm[m.reg]; uint32_t s0=0,s1=0; int i; for(i=0;i<8;i++){ int dd=dst[i]-s[i]; s0+=dd<0?-dd:dd; } for(i=8;i<16;i++){ int dd=dst[i]-s[i]; s1+=dd<0?-dd:dd; } uint64_t *o=(uint64_t*)dst; o[0]=s0; o[1]=s1; } break;
            case 0xd8: case 0xd9: /* psubusb/psubusw */ m=decode_modrm(c,&d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint8_t *dst=c->xmm[m.reg]; int i;
                  if(op2==0xd8){ for(i=0;i<16;i++){ int r=dst[i]-s[i]; dst[i]=r<0?0:r; } }
                  else { uint16_t *a=(uint16_t*)dst,*b=(uint16_t*)s; for(i=0;i<8;i++){ int r=a[i]-b[i]; a[i]=r<0?0:(uint16_t)r; } } } break;
            case 0xdc: case 0xdd: /* paddusb/paddusw */ m=decode_modrm(c,&d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint8_t *dst=c->xmm[m.reg]; int i;
                  if(op2==0xdc){ for(i=0;i<16;i++){ int r=dst[i]+s[i]; dst[i]=r>255?255:r; } }
                  else { uint16_t *a=(uint16_t*)dst,*b=(uint16_t*)s; for(i=0;i<8;i++){ int r=a[i]+b[i]; a[i]=r>65535?65535:(uint16_t)r; } } } break;
            case 0xe8: case 0xe9: /* psubsb/psubsw */ m=decode_modrm(c,&d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint8_t *dst=c->xmm[m.reg]; int i;
                  if(op2==0xe8){ for(i=0;i<16;i++){ int r=(int8_t)dst[i]-(int8_t)s[i]; dst[i]=(uint8_t)(int8_t)(r<-128?-128:r>127?127:r); } }
                  else { int16_t *a=(int16_t*)dst,*b=(int16_t*)s; for(i=0;i<8;i++){ int r=a[i]-b[i]; a[i]=(int16_t)(r<-32768?-32768:r>32767?32767:r); } } } break;
            case 0xec: case 0xed: /* paddsb/paddsw */ m=decode_modrm(c,&d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint8_t *dst=c->xmm[m.reg]; int i;
                  if(op2==0xec){ for(i=0;i<16;i++){ int r=(int8_t)dst[i]+(int8_t)s[i]; dst[i]=(uint8_t)(int8_t)(r<-128?-128:r>127?127:r); } }
                  else { int16_t *a=(int16_t*)dst,*b=(int16_t*)s; for(i=0;i<8;i++){ int r=a[i]+b[i]; a[i]=(int16_t)(r<-32768?-32768:r>32767?32767:r); } } } break;
            case 0xda: case 0xde: /* pminub/pmaxub */ m=decode_modrm(c,&d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint8_t *dst=c->xmm[m.reg]; int i;
                  if(op2==0xda){ for(i=0;i<16;i++) if(s[i]<dst[i]) dst[i]=s[i]; } else { for(i=0;i<16;i++) if(s[i]>dst[i]) dst[i]=s[i]; } } break;
            case 0xea: case 0xee: /* pminsw/pmaxsw */ m=decode_modrm(c,&d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; int16_t *a=(int16_t*)c->xmm[m.reg],*b=(int16_t*)s; int i;
                  if(op2==0xea){ for(i=0;i<8;i++) if(b[i]<a[i]) a[i]=b[i]; } else { for(i=0;i<8;i++) if(b[i]>a[i]) a[i]=b[i]; } } break;
            case 0xe0: case 0xe3: /* pavgb/pavgw */ m=decode_modrm(c,&d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint8_t *dst=c->xmm[m.reg]; int i;
                  if(op2==0xe0){ for(i=0;i<16;i++) dst[i]=(uint8_t)((dst[i]+s[i]+1)>>1); } else { uint16_t *a=(uint16_t*)dst,*b=(uint16_t*)s; for(i=0;i<8;i++) a[i]=(uint16_t)((a[i]+b[i]+1)>>1); } } break;
            case 0xf8: case 0xf9: case 0xfa: /* psubb/w/d */ m=decode_modrm(c,&d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint8_t *dst=c->xmm[m.reg]; int i;
                  if(op2==0xf8){ for(i=0;i<16;i++) dst[i]-=s[i]; }
                  else if(op2==0xf9){ uint16_t *a=(uint16_t*)dst,*b=(uint16_t*)s; for(i=0;i<8;i++) a[i]-=b[i]; }
                  else { uint32_t *a=(uint32_t*)dst,*b=(uint32_t*)s; for(i=0;i<4;i++) a[i]-=b[i]; } } break;
            case 0x54: /* andps */ m=decode_modrm(c,&d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; int i; for(i=0;i<16;i++) c->xmm[m.reg][i]&=s[i]; } break;
            case 0x56: /* orps */ m=decode_modrm(c,&d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; int i; for(i=0;i<16;i++) c->xmm[m.reg][i]|=s[i]; } break;
            case 0x14: case 0x15: /* unpcklps/unpckhps — 4-byte lane interleave */
                m=decode_modrm(c,&d); { uint8_t *s=m.is_reg?c->xmm[m.rm]:(uint8_t*)(uintptr_t)m.ea; uint32_t *dd=(uint32_t*)c->xmm[m.reg],*ss=(uint32_t*)s; uint32_t t[4]; int base=(op2==0x15)?2:0; t[0]=dd[base];t[1]=ss[base];t[2]=dd[base+1];t[3]=ss[base+1]; memcpy(dd,t,16);} break;
            case 0x77: break; /* emms */
            case 0xae: decode_modrm(c,&d); break; /* fxsave/lfence/sfence/mfence group: ignore */
            case 0x18: case 0x19: case 0x1a: case 0x1b: case 0x1c: case 0x1d: case 0x1e:
                decode_modrm(c,&d); break; /* prefetch/nop hints */
            case 0xa3: /* bt r/m, r */
                m=decode_modrm(c,&d); a=read_rm(c,&m,os); b=read_reg(c,m.reg,os)&(os*8-1);
                c->eflags=(c->eflags&~CF)|(((a>>b)&1)?CF:0); c->lf_size=0; break;
            case 0xab: /* bts */
                m=decode_modrm(c,&d); a=read_rm(c,&m,os); b=read_reg(c,m.reg,os)&(os*8-1);
                c->eflags=(c->eflags&~CF)|(((a>>b)&1)?CF:0); c->lf_size=0; write_rm(c,&m,os,a|(1u<<b)); break;
            case 0xba: /* grp8 bt/bts/btr/btc r/m, imm8 */
                m=decode_modrm(c,&d); a=read_rm(c,&m,os); { uint32_t bit=f8(&d)&(os*8-1);
                  c->eflags=(c->eflags&~CF)|(((a>>bit)&1)?CF:0); c->lf_size=0;
                  if(m.reg==5) write_rm(c,&m,os,a|(1u<<bit)); else if(m.reg==6) write_rm(c,&m,os,a&~(1u<<bit)); else if(m.reg==7) write_rm(c,&m,os,a^(1u<<bit)); }
                break;
            case 0xb0: case 0xb1: /* cmpxchg */
            {
                int sz=(op2&1)?os:1; m=decode_modrm(c,&d); a=read_rm(c,&m,sz); uint32_t acc=read_reg(c,EAX,sz);
                if (acc==a) { write_rm(c,&m,sz,read_reg(c,m.reg,sz)); set_lazy(c,K_SUB,acc,a,0,sz); }
                else { write_reg(c,EAX,sz,a); set_lazy(c,K_SUB,acc,a,acc-a,sz); }
                break;
            }
            case 0xc0: case 0xc1: /* xadd */
            {
                int sz=(op2&1)?os:1; m=decode_modrm(c,&d); a=read_rm(c,&m,sz); b=read_reg(c,m.reg,sz);
                write_rm(c,&m,sz,a+b); write_reg(c,m.reg,sz,a); set_lazy(c,K_ADD,a,b,a+b,sz); break;
            }
            case 0xc7: /* cmpxchg8b m64 (reg field 1) */
            {
                m = decode_modrm(c,&d);
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
            default: unimplemented(c,start,0x0f); return;
            }
            break;
        }

        default:
            unimplemented( c, start, op );
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

int wasm_x86_dispatch( struct x86cpu *c, uint32_t target )
{
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
        if (trace()) fprintf( stderr, "wasm_x86: syscall %04x returned eax=%08x ret_eip=%08x\n", num, c->regs[EAX], ret_eip );
        c->eip = ret_eip;
        return 1;
    }
    if (target == addr_unixcall)
    {
        /* i386 __wine_unix_call_dispatcher stack on entry (after the call):
         * [esp]=ret_eip, [esp+4]=handle, [esp+8]=code, [esp+12]=args_ptr.
         * handle is the module's unixlib function table; code indexes it. */
        uint32_t ret_eip = pop32( c );
        unixlib_handle_t handle = rd32( c->regs[ESP] + 0 );
        unsigned int code = rd32( c->regs[ESP] + 4 );
        void *args = (void *)(uintptr_t)rd32( c->regs[ESP] + 8 );
        const unixlib_entry_t *funcs = (const unixlib_entry_t *)(uintptr_t)handle;
        if (trace()) fprintf( stderr, "wasm_x86: unix_call handle=%llx code=%u args=%p\n",
                              (unsigned long long)handle, code, args );
        c->regs[EAX] = funcs[code]( args );
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
