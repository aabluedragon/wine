#!/usr/bin/env python3
# Static x86 -> C binary translator for the webwine interpreter.
#
# Disassembles ranges of netduke32.exe's .text with capstone, splits each into
# basic blocks, and emits C block functions that reuse the interpreter's own
# helpers (c->regs[], rd8/wr8, set_lazy, cond, push32/pop32, read_reg/write_reg).
# The output is #included into wasm_x86.c so the blocks make DIRECT calls to the
# helpers (no cross-module boundary) - this is the whole point vs a runtime JIT.
#
# An instruction the translator doesn't implement does NOT break the block: it
# emits a jit_step1() call that runs that one instruction through the
# interpreter and then carries on, so the surrounding integer code stays
# translated and the block stays chained.  jit_step1 returns 0 if the
# instruction did not fall through as expected (it branched or trapped), and the
# block bails out to the main loop - which makes this safe for ANY opcode.
#
#   python3 x86toc.py netduke32.exe out.c  0x506c30-0x507000  0x408000-0x409000 ...
import sys, capstone
from capstone import x86 as X

EXE, OUT = sys.argv[1], sys.argv[2]
RANGES = []
HOOKS = set()   # native-hooked guest addresses: must be block boundaries and are
                # never themselves translated, so run()'s nat check always fires.
EXTRA_ENTRIES = set()  # explicitly proven indirect entries
SLIDE_SYMBOL = 'nd_slide'
GEN_PREFIX = ''
NO_FP = False
CFG_EXPORTS = False
for a in sys.argv[3:]:
    if a.startswith('--hooks='):
        for h in a[len('--hooks='):].split(','):
            if h: HOOKS.add(int(h, 16))
        continue
    if a.startswith('--entries='):
        for e in a[len('--entries='):].split(','):
            if e: EXTRA_ENTRIES.add(int(e, 16))
        continue
    if a.startswith('--slide-symbol='):
        SLIDE_SYMBOL = a[len('--slide-symbol='):]
        continue
    if a.startswith('--prefix='):
        GEN_PREFIX = a[len('--prefix='):]
        continue
    if a == '--no-fp':
        NO_FP = True
        continue
    if a == '--cfg-exports':
        CFG_EXPORTS = True
        continue
    lo, hi = a.split('-'); RANGES.append((int(lo, 16), int(hi, 16)))

# --- read the PE .text (coff-i386): map file offset for VAs -------------------
data = open(EXE, 'rb').read()
# PE header: e_lfanew at 0x3c
pe = int.from_bytes(data[0x3c:0x40], 'little')
assert data[pe:pe+4] == b'PE\0\0'
nsec = int.from_bytes(data[pe+6:pe+8], 'little')
opt = pe + 24
image_base = int.from_bytes(data[opt+28:opt+32], 'little')   # 0x400000
sec = opt + int.from_bytes(data[pe+20:pe+22], 'little')       # size of optional header
sections = []
for i in range(nsec):
    s = sec + i*40
    name = data[s:s+8].rstrip(b'\0')
    vsize = int.from_bytes(data[s+8:s+12], 'little')
    va = image_base + int.from_bytes(data[s+12:s+16], 'little')
    rawsz = int.from_bytes(data[s+16:s+20], 'little')
    raw = int.from_bytes(data[s+20:s+24], 'little')
    sections.append((name, va, vsize, raw, rawsz))
image_end = max(va + vsize for _, va, vsize, _, _ in sections)

def va_bytes(va, n):
    for name, sva, vsz, raw, rawsz in sections:
        if sva <= va < sva + vsz:
            off = raw + (va - sva)
            return data[off:off+n]
    return b''

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.detail = True

def export_entries():
    """Return exported code RVAs as VA seeds for a DLL CFG walk."""
    exp_rva = int.from_bytes(data[opt+96:opt+100], 'little')
    if not exp_rva: return []
    exp = image_base + exp_rva
    def u32(va):
        b = va_bytes(va, 4)
        return int.from_bytes(b, 'little') if len(b) == 4 else 0
    nname, funcs, names, ords = (u32(exp+24), u32(exp+28), u32(exp+32), u32(exp+36))
    out = []
    for i in range(nname):
        ordinal = int.from_bytes(va_bytes(image_base + ords + i*2, 2), 'little')
        rva = u32(image_base + funcs + ordinal*4)
        va = image_base + rva
        if any(lo <= va < hi for lo, hi in RANGES): out.append(va)
    return out

REG = {  # capstone reg id -> (index, size).  8-bit high regs use index+4.
 X.X86_REG_EAX:(0,4), X.X86_REG_ECX:(1,4), X.X86_REG_EDX:(2,4), X.X86_REG_EBX:(3,4),
 X.X86_REG_ESP:(4,4), X.X86_REG_EBP:(5,4), X.X86_REG_ESI:(6,4), X.X86_REG_EDI:(7,4),
 X.X86_REG_AX:(0,2), X.X86_REG_CX:(1,2), X.X86_REG_DX:(2,2), X.X86_REG_BX:(3,2),
 X.X86_REG_SP:(4,2), X.X86_REG_BP:(5,2), X.X86_REG_SI:(6,2), X.X86_REG_DI:(7,2),
 X.X86_REG_AL:(0,1), X.X86_REG_CL:(1,1), X.X86_REG_DL:(2,1), X.X86_REG_BL:(3,1),
 X.X86_REG_AH:(4,1), X.X86_REG_CH:(5,1), X.X86_REG_DH:(6,1), X.X86_REG_BH:(7,1),
}
SEG = { X.X86_REG_FS:'(int)c->fs_base', X.X86_REG_GS:'(int)c->gs_base' }

class Unhandled(Exception): pass

def ea_expr(op):
    """C expression for a memory operand's effective address (uint32_t)."""
    m = op.mem
    parts = []
    if m.base != 0:
        if m.base not in REG or REG[m.base][1] != 4: raise Unhandled('ea base')
        parts.append('c->regs[%d]' % REG[m.base][0])
    if m.index != 0:
        if m.index not in REG or REG[m.index][1] != 4: raise Unhandled('ea index')
        parts.append('(c->regs[%d]<<%d)' % (REG[m.index][0], {1:0,2:1,4:2,8:3}[m.scale]))
    # A displacement is absolute whenever there is no base register.  This
    # includes SIB forms such as [index*4 + table]; the index must not trick
    # relocation into treating the table VA as a plain constant.
    absolute = m.base == 0
    if m.disp != 0 or absolute:
        disp = '%du' % (m.disp & 0xffffffff)
        if absolute and m.disp:
            disp = '(%s + (uint32_t)%s)' % (disp, SLIDE_SYMBOL)
        parts.append(disp)
    seg = ''
    if m.segment in SEG: seg = ' + (uint32_t)%s' % SEG[m.segment]
    elif m.segment != 0: raise Unhandled('ea seg')
    return '(' + ' + '.join(parts) + seg + ')'

def rd_op(insn, oi):
    """C expression reading operand oi (as uint32_t, zero-extended to its size)."""
    op = insn.operands[oi]
    if op.type == X.X86_OP_REG:
        idx, sz = REG.get(op.reg, (None,None))
        if idx is None: raise Unhandled('reg %s' % insn.reg_name(op.reg))
        if sz == 4: return 'c->regs[%d]' % idx, 4
        return 'read_reg(c,%d,%d)' % (idx, sz), sz
    if op.type == X.X86_OP_IMM:
        imm = op.imm & 0xffffffff
        # PE relocations also cover address-valued immediates (for example a
        # pointer to a DLL's const table).  The translator cannot cheaply carry
        # the relocation directory through every instruction, but an immediate
        # inside this image is unambiguously an image address for these Wine
        # PEs; ordinary integer constants are far outside this range.
        if image_base <= imm < image_end:
            return '(%du + (uint32_t)%s)' % (imm, SLIDE_SYMBOL), op.size
        return '%du' % imm, op.size
    if op.type == X.X86_OP_MEM:
        sz = op.size
        ea = ea_expr(op)
        return ('rd8' if sz==1 else 'rd16' if sz==2 else 'rd32')+'('+ea+')', sz
    raise Unhandled('rd optype')

def wr_op(insn, oi, valexpr):
    """C statement writing valexpr into operand oi."""
    op = insn.operands[oi]
    if op.type == X.X86_OP_REG:
        idx, sz = REG.get(op.reg, (None,None))
        if idx is None: raise Unhandled('wreg')
        if sz == 4: return 'c->regs[%d] = %s;' % (idx, valexpr)
        return 'write_reg(c,%d,%d,%s);' % (idx, sz, valexpr)
    if op.type == X.X86_OP_MEM:
        sz = op.size; ea = ea_expr(op)
        fn = 'wr8' if sz==1 else 'wr16' if sz==2 else 'wr32'
        return '%s(%s, %s);' % (fn, ea, valexpr)
    raise Unhandled('wr optype')

def opsize(insn):
    return insn.operands[0].size if insn.operands else 4

def xmm_idx(reg):
    name = md.reg_name(reg)
    if name.startswith('xmm') and name[3:].isdigit() and int(name[3:]) < 8:
        return int(name[3:])
    raise Unhandled('xmm reg')

def ymm_idx(reg):
    name = md.reg_name(reg)
    if name.startswith('ymm') and name[3:].isdigit() and int(name[3:]) < 8:
        return int(name[3:])
    raise Unhandled('ymm reg')

def is_vec(reg):
    return md.reg_name(reg).startswith(('xmm','mm'))

def mm_idx(reg):
    name=md.reg_name(reg)
    if name.startswith('mm') and name[2:].isdigit() and int(name[2:])<8: return int(name[2:])
    raise Unhandled('mm reg')

def xmm_mem(op):
    if op.type != X.X86_OP_MEM: raise Unhandled('xmm memory')
    return ea_expr(op)

def st_idx(reg):
    name = md.reg_name(reg)
    if name in ('st', 'st(0)'): return 0
    if name.startswith('st(') and name.endswith(')') and name[3:-1].isdigit():
        n = int(name[3:-1])
        if n < 8: return n
    raise Unhandled('x87 reg')

ARITH = {  # id -> (C expr for result given a,b ; flag kind ; writes_back)
 X.X86_INS_ADD:('(a+b)','K_ADD',True), X.X86_INS_SUB:('(a-b)','K_SUB',True),
 X.X86_INS_OR :('(a|b)','K_LOGIC',True), X.X86_INS_AND:('(a&b)','K_LOGIC',True),
 X.X86_INS_XOR:('(a^b)','K_LOGIC',True),
 X.X86_INS_CMP:('(a-b)','K_SUB',False), X.X86_INS_TEST:('(a&b)','K_LOGIC',False),
}
CC = {  # capstone jcc id -> interpreter cc code (0..15, as used by cond())
 X.X86_INS_JO:0, X.X86_INS_JNO:1, X.X86_INS_JB:2, X.X86_INS_JAE:3,
 X.X86_INS_JE:4, X.X86_INS_JNE:5, X.X86_INS_JBE:6, X.X86_INS_JA:7,
 X.X86_INS_JS:8, X.X86_INS_JNS:9, X.X86_INS_JP:10, X.X86_INS_JNP:11,
 X.X86_INS_JL:12, X.X86_INS_JGE:13, X.X86_INS_JLE:14, X.X86_INS_JG:15,
}
# modrm.reg field selecting the shift kind in do_shift()
SHIFT = { X.X86_INS_SHL:4, X.X86_INS_SAL:6, X.X86_INS_SHR:5, X.X86_INS_SAR:7,
          X.X86_INS_ROL:0, X.X86_INS_ROR:1 }
# cc suffix -> cond() code, for cmovcc/setcc (parsed from the mnemonic)
CCSUF = {'o':0,'no':1,'b':2,'c':2,'nae':2,'ae':3,'nb':3,'nc':3,'e':4,'z':4,
 'ne':5,'nz':5,'be':6,'na':6,'a':7,'nbe':7,'s':8,'ns':9,'p':10,'pe':10,
 'np':11,'po':11,'l':12,'nge':12,'ge':13,'nl':13,'le':14,'ng':14,'g':15,'nle':15}

def translate_insn(insn):
    """Return (list of C statements, control) where control is None for
    fall-through or ('branch',) if the insn ends the block (sets eip+returns)."""
    I = insn.id; ops = insn.operands
    S = []
    mn = insn.mnemonic
    if NO_FP and (mn.startswith('f') or any(
            op.type == X.X86_OP_REG and md.reg_name(op.reg).startswith(('xmm', 'ymm', 'mm', 'st'))
            for op in ops)):
        raise Unhandled('fp/simd disabled')
    if I == X.X86_INS_NOP: return [], None
    if mn in ('cld','std'):
        S.append('c->eflags %s DF;' % ('&=' + '~' if mn=='cld' else '|=')); return S, None
    if mn in ('cpuid','rdtsc'):
        if mn=='cpuid': S.extend(['c->regs[EAX]=0;','c->regs[EBX]=0;','c->regs[ECX]=0;','c->regs[EDX]=0;'])
        else: S.extend(['c->regs[EAX]=0;','c->regs[EDX]=0;'])
        return S, None
    if mn == 'xgetbv':
        S.extend(['if(c->regs[ECX]==0){c->regs[EAX]=7;c->regs[EDX]=0;} else {c->regs[EAX]=0;c->regs[EDX]=0;}'])
        return S, None
    if mn in ('cmpxchg8b','lock cmpxchg8b'):
        if len(ops)!=1 or ops[0].type!=X.X86_OP_MEM: raise Unhandled('cmpxchg8b form')
        S.append('jit_cmpxchg8b(c,%s);' % xmm_mem(ops[0])); return S, None
    if mn in ('wait','emms','prefetchnta','prefetcht0','prefetcht1','prefetcht2','prefetchw'):
        return [], None
    if I == X.X86_INS_MOV:
        v, sz = rd_op(insn, 1); S.append(wr_op(insn, 0, v)); return S, None
    if I in (X.X86_INS_MOVZX, X.X86_INS_MOVSX):
        v, sz = rd_op(insn, 1)
        if I == X.X86_INS_MOVSX:
            v = '(uint32_t)(int32_t)%s(%s)' % ('(int8_t)' if sz==1 else '(int16_t)', v)
        S.append(wr_op(insn, 0, v)); return S, None
    if I == X.X86_INS_LEA:
        S.append(wr_op(insn, 0, ea_expr(ops[1]))); return S, None
    if I in ARITH:
        expr, kind, wb = ARITH[I]; sz = opsize(insn)
        av, _ = rd_op(insn, 0); bv, _ = rd_op(insn, 1)
        S.append('{ uint32_t a=%s, b=%s, r=%s;' % (av, bv, expr))
        if wb: S.append('  ' + wr_op(insn, 0, 'r'))
        S.append('  set_lazy(c,%s,a,b,r,%d); }' % (kind, sz)); return S, None
    if I in (X.X86_INS_INC, X.X86_INS_DEC):
        sz = opsize(insn); av, _ = rd_op(insn, 0)
        d = '+1' if I == X.X86_INS_INC else '-1'
        S.append('{ uint32_t a=%s, r=a%s;' % (av, d))
        S.append('  ' + wr_op(insn, 0, 'r'))
        S.append('  set_lazy(c,%s,a,1,r,%d); }' % ('K_INC' if I==X.X86_INS_INC else 'K_DEC', sz)); return S, None
    if I in SHIFT:
        sz = opsize(insn); av, _ = rd_op(insn, 0)
        cv = rd_op(insn, 1)[0] if len(ops) >= 2 else '1u'
        S.append('{ uint32_t a=%s, r=do_shift(c,%d,a,%s,%d);' % (av, SHIFT[I], cv, sz))
        S.append('  ' + wr_op(insn, 0, 'r') + ' }'); return S, None
    if I in (X.X86_INS_SHLD, X.X86_INS_SHRD):
        sz = opsize(insn); dv, _ = rd_op(insn, 0); sv, _ = rd_op(insn, 1)
        cv = rd_op(insn, 2)[0]
        left = 1 if I == X.X86_INS_SHLD else 0
        S.append('{ uint32_t d=%s, s=%s, r=do_dshift(c,%d,d,s,%s,%d);' % (dv, sv, left, cv, sz))
        S.append('  ' + wr_op(insn, 0, 'r') + ' }'); return S, None
    if I == X.X86_INS_IMUL and len(ops) >= 2:     # 2-op or 3-op (no flags, like the interp)
        sz = opsize(insn); av, _ = rd_op(insn, 0)
        if len(ops) == 2: bv, _ = rd_op(insn, 1)
        else:             av, _ = rd_op(insn, 1); bv, _ = rd_op(insn, 2)
        S.append(wr_op(insn, 0, '(uint32_t)((int32_t)%s*(int32_t)%s)' % (av, bv))); return S, None
    if I == X.X86_INS_NEG:
        sz = opsize(insn); av, _ = rd_op(insn, 0)
        S.append('{ uint32_t a=%s, r=(uint32_t)(-(int32_t)a);' % av)
        S.append('  ' + wr_op(insn, 0, 'r'))
        S.append('  set_lazy(c,K_SUB,0,a,r,%d); }' % sz); return S, None
    if I == X.X86_INS_NOT:
        av, _ = rd_op(insn, 0); S.append(wr_op(insn, 0, '~%s' % av)); return S, None
    if I in (X.X86_INS_ADC, X.X86_INS_SBB):
        sz = opsize(insn); av, _ = rd_op(insn, 0); bv, _ = rd_op(insn, 1)
        op = '+' if I == X.X86_INS_ADC else '-'; kind = 'K_ADC' if I == X.X86_INS_ADC else 'K_SBB'
        S.append('{ int ci=lf_cf(c); uint32_t a=%s, b=%s, r=a%sb%sci;' % (av, bv, op, op))
        S.append('  ' + wr_op(insn, 0, 'r'))
        S.append('  set_lazy(c,%s,a,b,r,%d); c->lf_cin=ci; }' % (kind, sz)); return S, None
    if I in (X.X86_INS_CMOVA, X.X86_INS_CMOVAE, X.X86_INS_CMOVB, X.X86_INS_CMOVBE,
             X.X86_INS_CMOVE, X.X86_INS_CMOVG, X.X86_INS_CMOVGE, X.X86_INS_CMOVL,
             X.X86_INS_CMOVLE, X.X86_INS_CMOVNE, X.X86_INS_CMOVNO, X.X86_INS_CMOVNP,
             X.X86_INS_CMOVNS, X.X86_INS_CMOVO, X.X86_INS_CMOVP, X.X86_INS_CMOVS):
        cc = CCSUF[mn[4:]]; av, _ = rd_op(insn, 1)
        S.append('{ uint32_t a=%s; if (cond(c,%d)) %s }' % (av, cc, wr_op(insn, 0, 'a'))); return S, None
    if mn.startswith('set') and mn[3:] in CCSUF:
        cc = CCSUF[mn[3:]]
        S.append(wr_op(insn, 0, '(cond(c,%d)?1u:0u)' % cc)); return S, None
    if I in (X.X86_INS_MUL, X.X86_INS_DIV, X.X86_INS_IDIV) or (I == X.X86_INS_IMUL and len(ops) == 1):
        regf = {X.X86_INS_MUL:4, X.X86_INS_IMUL:5, X.X86_INS_DIV:6, X.X86_INS_IDIV:7}[I]
        av, _ = rd_op(insn, 0)
        S.append('do_muldiv(c,%d,%s,%d);' % (regf, av, opsize(insn))); return S, None
    if I == X.X86_INS_CWDE:
        S.append('c->regs[0] = (int32_t)(int16_t)(c->regs[0] & 0xffff);'); return S, None
    if mn == 'cbw':
        S.append('c->regs[0]=(c->regs[0]&0xffff0000u)|(uint16_t)(int16_t)(uint8_t)c->regs[0];'); return S, None
    if mn == 'pushfd':
        S.append('push32(c,get_flags(c));'); return S, None
    if mn == 'popfd':
        S.append('c->eflags=pop32(c); c->lf_size=0;'); return S, None
    if mn == 'sahf':
        S.append('{ uint32_t a=(c->regs[0]>>8)&0xff; c->eflags=(c->eflags&~(SF|ZF|AF|PF|CF))|((a&0x80)?SF:0)|((a&0x40)?ZF:0)|((a&0x10)?AF:0)|((a&4)?PF:0)|((a&1)?CF:0); c->lf_size=0; }'); return S, None
    if mn in ('pause','vzeroupper'):
        return S, None
    if I in (X.X86_INS_BSF, X.X86_INS_BSR):
        av, _ = rd_op(insn, 1)
        idx = '__builtin_ctz(a)' if I == X.X86_INS_BSF else '(31 - __builtin_clz(a))'
        S.append('{ uint32_t a=%s; if(a){ %s c->eflags&=~ZF; } else c->eflags|=ZF; c->lf_size=0; }'
                 % (av, wr_op(insn, 0, idx))); return S, None
    if mn == 'tzcnt':
        av, _ = rd_op(insn, 1); sz = opsize(insn); limit = sz * 8
        S.append('{ uint32_t a=%s, r=a?(uint32_t)__builtin_ctz(a):%d; %s'
                 % (av, limit, wr_op(insn, 0, 'r')))
        S.append(' c->eflags=(c->eflags&~(ZF|CF))|(r==0?ZF:0)|(a==0?CF:0); c->lf_size=0; }')
        return S, None
    if I in (X.X86_INS_BT, X.X86_INS_BTS, X.X86_INS_BTR, X.X86_INS_BTC):
        sz = opsize(insn); av, _ = rd_op(insn, 0); bv, _ = rd_op(insn, 1)
        S.append('{ uint32_t a=%s, b=%s&(%d); c->eflags=(c->eflags&~CF)|(((a>>b)&1)?CF:0); c->lf_size=0;'
                 % (av, bv, sz*8-1))
        upd = {X.X86_INS_BTS:'a|(1u<<b)', X.X86_INS_BTR:'a&~(1u<<b)', X.X86_INS_BTC:'a^(1u<<b)'}.get(I)
        if upd: S.append('  ' + wr_op(insn, 0, '(%s)' % upd))
        S.append('}'); return S, None
    if I == X.X86_INS_BSWAP:
        idx, _ = REG[ops[0].reg]
        S.append('{ uint32_t v=c->regs[%d]; c->regs[%d]=((v>>24)&0xff)|((v>>8)&0xff00)|((v<<8)&0xff0000)|((v<<24)&0xff000000); }'
                 % (idx, idx)); return S, None
    if I == X.X86_INS_XCHG:
        v0, _ = rd_op(insn, 0); v1, _ = rd_op(insn, 1)
        S.append('{ uint32_t t0=%s, t1=%s; %s %s }'
                 % (v0, v1, wr_op(insn, 0, 't1'), wr_op(insn, 1, 't0'))); return S, None
    if I == X.X86_INS_XADD:
        sz = opsize(insn); av, _ = rd_op(insn, 0); bv, _ = rd_op(insn, 1)
        S.append('{ uint32_t a=%s, b=%s; %s %s set_lazy(c,K_ADD,a,b,a+b,%d); }'
                 % (av, bv, wr_op(insn, 0, 'a+b'), wr_op(insn, 1, 'a'), sz)); return S, None
    if I == X.X86_INS_CMPXCHG:
        sz = opsize(insn); rmv, _ = rd_op(insn, 0); regv, _ = rd_op(insn, 1)
        eax = 'read_reg(c,0,%d)' % sz
        S.append('{ uint32_t a=%s, acc=%s;' % (rmv, eax))
        S.append('  if(acc==a){ %s set_lazy(c,K_SUB,acc,a,0,%d); }' % (wr_op(insn, 0, regv), sz))
        S.append('  else { write_reg(c,0,%d,a); set_lazy(c,K_SUB,acc,a,acc-a,%d); } }' % (sz, sz)); return S, None
    # x87 stack movement.  Use the interpreter's fp_* primitives so TOP and
    # the physical register mapping remain identical to the fallback path.
    if mn == 'fld':
        if len(ops)!=1: raise Unhandled('fld form')
        if ops[0].type == X.X86_OP_REG:
            S.append('fp_push(c,fp_get(c,%d));' % st_idx(ops[0].reg))
        elif ops[0].type == X.X86_OP_MEM:
            ea=xmm_mem(ops[0]); sz=ops[0].size
            if sz==4: S.append('fp_push(c,rdf32(%s));' % ea)
            elif sz==8: S.append('fp_push(c,rdf64(%s));' % ea)
            elif sz==10: S.append('fp_push(c,rd80(%s));' % ea)
            else: raise Unhandled('fld size')
        else: raise Unhandled('fld source')
        return S, None
    if mn in ('fst','fstp'):
        if len(ops)!=1: raise Unhandled('fst form')
        pop = mn == 'fstp'
        if ops[0].type == X.X86_OP_REG:
            i=st_idx(ops[0].reg); S.append('fp_set(c,%d,fp_get(c,0));' % i)
            if pop: S.append('fp_pop(c);')
        elif ops[0].type == X.X86_OP_MEM:
            ea=xmm_mem(ops[0]); sz=ops[0].size
            if sz==4: w='wrf32(%s,fp_get(c,0));' % ea
            elif sz==8: w='wrf64(%s,fp_get(c,0));' % ea
            elif sz==10: w='wr80(%s,fp_get(c,0));' % ea
            else: raise Unhandled('fst size')
            S.append(w)
            if pop: S.append('fp_pop(c);')
        else: raise Unhandled('fst destination')
        return S, None
    if mn in ('fild','fist','fistp'):
        if len(ops)!=1 or ops[0].type!=X.X86_OP_MEM: raise Unhandled('fist form')
        ea=xmm_mem(ops[0]); sz=ops[0].size; pop=mn=='fistp'
        if mn=='fild':
            if sz==2: S.append('fp_push(c,(double)(int16_t)rd16(%s));' % ea)
            elif sz==4: S.append('fp_push(c,(double)(int32_t)rd32(%s));' % ea)
            elif sz==8: S.append('fp_push(c,(double)(int64_t)((uint64_t)rd32(%s)|((uint64_t)rd32(%s+4)<<32)));' % (ea,ea))
            else: raise Unhandled('fild size')
        else:
            if sz==2: S.append('wr16(%s,(uint16_t)(int16_t)lrint(fp_get(c,0)));' % ea)
            elif sz==4: S.append('wr32(%s,(uint32_t)(int32_t)lrint(fp_get(c,0)));' % ea)
            elif sz==8:
                S.append('{ int64_t v=(int64_t)llrint(fp_get(c,0)); wr32(%s,(uint32_t)v); wr32(%s+4,(uint32_t)((uint64_t)v>>32)); }' % (ea,ea))
            else: raise Unhandled('fist size')
            if pop: S.append('fp_pop(c);')
        return S, None
    if mn == 'fxch':
        if len(ops)==0: i=1
        elif len(ops)==1 and ops[0].type==X.X86_OP_REG: i=st_idx(ops[0].reg)
        elif len(ops)==2 and ops[0].type==X.X86_OP_REG and ops[1].type==X.X86_OP_REG: i=st_idx(ops[1].reg)
        else: raise Unhandled('fxch form')
        S.append('{ double t=fp_get(c,0); fp_set(c,0,fp_get(c,%d)); fp_set(c,%d,t); }' % (i,i)); return S, None
    if mn in ('fld1','fldz'):
        if ops: raise Unhandled('constant x87 form')
        S.append('fp_push(c,%s);' % ('1.0' if mn=='fld1' else '0.0')); return S, None
    if mn == 'fchs':
        if ops: raise Unhandled('fchs form')
        S.append('fp_set(c,0,-fp_get(c,0));'); return S, None
    if mn in ('fabs','frndint','fsqrt'):
        if ops: raise Unhandled('unary x87 form')
        expr={'fabs':'fabs(fp_get(c,0))','frndint':'rint(fp_get(c,0))','fsqrt':'sqrt(fp_get(c,0))'}[mn]
        S.append('fp_set(c,0,%s);' % expr); return S, None
    if mn.startswith('fcmov'):
        cc={'fcmovb':2,'fcmove':4,'fcmovbe':6,'fcmovu':10,'fcmovnb':3,'fcmovne':5,'fcmovnbe':7,'fcmovnu':11}.get(mn)
        if cc is None or len(ops)!=2 or ops[0].type!=X.X86_OP_REG or ops[1].type!=X.X86_OP_REG: raise Unhandled('fcmov form')
        S.append('if(cond(c,%d)) fp_set(c,0,fp_get(c,%d));' % (cc,st_idx(ops[1].reg))); return S, None
    if mn == 'fnstsw':
        v='(uint16_t)(c->fpsw|((c->fptop&7)<<11))'
        if len(ops)==1 and ops[0].type==X.X86_OP_MEM:S.append('wr16(%s,%s);' % (xmm_mem(ops[0]),v))
        elif len(ops)==1 and ops[0].type==X.X86_OP_REG and md.reg_name(ops[0].reg)=='ax':S.append('write_reg(c,EAX,2,%s);' % v)
        else:raise Unhandled('fnstsw form')
        return S,None
    if mn in ('fldcw','fnstcw'):
        if len(ops)!=1 or ops[0].type!=X.X86_OP_MEM or ops[0].size!=2: raise Unhandled('x87 control form')
        ea=xmm_mem(ops[0]); S.append('c->fpcw=rd16(%s);' % ea if mn=='fldcw' else 'wr16(%s,c->fpcw);' % ea); return S, None
    if mn in ('fcomi','fucomi','fcomip','fucomip','fcompi','fucompi'):
        if len(ops)==1 and ops[0].type==X.X86_OP_REG:
            i=st_idx(ops[0].reg); S.append('fp_compare_eflags(c,fp_get(c,0),fp_get(c,%d));' % i)
            if mn.endswith('p') or mn in ('fcompi','fucompi'): S.append('fp_pop(c);')
            return S, None
        raise Unhandled('x87 compare form')
    if mn == 'fxam':
        if ops: raise Unhandled('fxam operands')
        S.append('jit_x87_fxam(c);'); return S, None
    if mn in ('f2xm1','fscale','fprem','fprem1','fsin','fcos'):
        if ops: raise Unhandled('x87 math operands')
        op={'f2xm1':0,'fscale':1,'fprem':2,'fprem1':2,'fsin':3,'fcos':4}[mn]
        S.append('jit_x87_math(c,%d);' % op); return S, None
    if mn in ('fldpi','fldl2e'):
        if ops: raise Unhandled('x87 constant operands')
        S.append('fp_push(c,%s);' % ('3.141592653589793' if mn=='fldpi' else '1.4426950408889634'))
        return S, None
    if mn in ('movlps','movhps'):
        if len(ops)!=2: raise Unhandled('xmm half memory form')
        high=1 if mn=='movhps' else 0
        if ops[0].type==X.X86_OP_REG and ops[1].type==X.X86_OP_MEM:
            S.append('jit_xmm_loadhalf(c,%d,%s,%d);' % (xmm_idx(ops[0].reg),xmm_mem(ops[1]),high))
        elif ops[0].type==X.X86_OP_MEM and ops[1].type==X.X86_OP_REG:
            S.append('jit_xmm_storehalf(c,%d,%s,%d);' % (xmm_idx(ops[1].reg),xmm_mem(ops[0]),high))
        else: raise Unhandled('xmm half operands')
        return S, None
    if mn == 'movd':
        if len(ops)!=2: raise Unhandled('movd form')
        if ops[0].type==X.X86_OP_REG and is_vec(ops[0].reg) and ops[1].type in (X.X86_OP_REG,X.X86_OP_MEM):
            dst=mm_idx(ops[0].reg) if md.reg_name(ops[0].reg).startswith('mm') else xmm_idx(ops[0].reg); v,_=rd_op(insn,1); S.append('jit_xmm_movd_load(c,%d,%s);' % (dst,v))
        elif ops[0].type==X.X86_OP_MEM and ops[1].type==X.X86_OP_REG:
            src=mm_idx(ops[1].reg) if md.reg_name(ops[1].reg).startswith('mm') else xmm_idx(ops[1].reg)
            S.append('jit_xmm_movd_store(c,%d,%s);' % (src,xmm_mem(ops[0])))
        elif ops[0].type==X.X86_OP_REG and ops[1].type==X.X86_OP_REG and not md.reg_name(ops[0].reg).startswith('xmm'):
            idx,_=REG.get(ops[0].reg,(None,None))
            if idx is None: raise Unhandled('movd general destination')
            src=mm_idx(ops[1].reg) if md.reg_name(ops[1].reg).startswith('mm') else xmm_idx(ops[1].reg)
            S.append('c->regs[%d]=jit_xmm_movd_value(c,%d);' % (idx,src))
        else: raise Unhandled('movd operands')
        return S, None
    if mn == 'movq':
        if len(ops)!=2: raise Unhandled('movq form')
        if ops[0].type==X.X86_OP_REG and ops[1].type==X.X86_OP_REG:
            d=mm_idx(ops[0].reg) if md.reg_name(ops[0].reg).startswith('mm') else xmm_idx(ops[0].reg);s=mm_idx(ops[1].reg) if md.reg_name(ops[1].reg).startswith('mm') else xmm_idx(ops[1].reg)
            S.append('jit_mmx_movq_load(c,%d,c->xmm[%d]);' % (d,s) if md.reg_name(ops[0].reg).startswith('mm') else 'jit_xmm_movq_load(c,%d,c->xmm[%d]);' % (d,s))
        elif ops[0].type==X.X86_OP_REG and ops[1].type==X.X86_OP_MEM:
            d=mm_idx(ops[0].reg) if md.reg_name(ops[0].reg).startswith('mm') else xmm_idx(ops[0].reg); S.append('jit_xmm_load(c,%d,%s,8,1);' % (d,xmm_mem(ops[1])))
        elif ops[0].type==X.X86_OP_MEM and ops[1].type==X.X86_OP_REG:
            s=mm_idx(ops[1].reg) if md.reg_name(ops[1].reg).startswith('mm') else xmm_idx(ops[1].reg); S.append('jit_mmx_movq_store(c,%d,%s);' % (s,xmm_mem(ops[0])))
        else: raise Unhandled('movq operands')
        return S, None
    if mn == 'movdq2q':
        if len(ops)!=2 or md.reg_name(ops[0].reg).startswith('mm') is False or md.reg_name(ops[1].reg).startswith('xmm') is False:
            raise Unhandled('movdq2q form')
        S.append('jit_mmx_movq_load(c,%d,c->xmm[%d]);' % (mm_idx(ops[0].reg),xmm_idx(ops[1].reg)))
        return S,None
    if mn in ('fadd','fmul','fsub','fsubr','fdiv','fdivr','faddp','fmulp','fsubp','fsubrp','fdivp','fdivrp'):
        if len(ops)==1 and ops[0].type==X.X86_OP_REG:
            base=mn.rstrip('p'); i=st_idx(ops[0].reg)
            op={'fadd':0,'fmul':1,'fsub':2,'fsubr':2,'fdiv':3,'fdivr':3}[base]
            rev=1 if base in ('fsubr','fdivr') else 0
            if mn.endswith('p'): S.append('jit_x87_bin(c,%d,%d,fp_get(c,0),1,%d);' % (op,i,rev))
            else: S.append('jit_x87_bin(c,%d,0,fp_get(c,%d),0,%d);' % (op,i,rev))
            return S, None
        if len(ops)==1 and ops[0].type==X.X86_OP_MEM:
            ea=xmm_mem(ops[0]); sz=ops[0].size
            src = 'rdf32(%s)' % ea if sz==4 else 'rdf64(%s)' % ea
            if sz not in (4,8): raise Unhandled('x87 arithmetic size')
            op={'fadd':0,'fmul':1,'fsub':2,'fsubr':2,'fdiv':3,'fdivr':3}[mn]
            rev=1 if mn in ('fsubr','fdivr') else 0
            S.append('jit_x87_bin(c,%d,0,%s,%d,%d);' % (op,src,0,rev)); return S, None
        if len(ops)==2 and all(o.type==X.X86_OP_REG for o in ops):
            dst=st_idx(ops[0].reg); src=st_idx(ops[1].reg)
            base=mn.rstrip('p'); op={'fadd':0,'fmul':1,'fsub':2,'fsubr':2,'fdiv':3,'fdivr':3}[base]
            rev=1 if base in ('fsubr','fdivr') else 0
            # x87 pop forms compute into the named destination, then pop ST0.
            S.append('jit_x87_bin(c,%d,%d,fp_get(c,%d),%d,%d);' % (op,dst,src,1 if mn.endswith('p') else 0,rev)); return S, None
        raise Unhandled('x87 arithmetic form')
    # Safe, high-frequency SSE/SSE2 data movement.  Arithmetic remains
    # interpreted until its floating-point rounding/NaN behavior is covered.
    if mn in ('movss','movsd','movups','movaps','movdqa','movdqu') and ops and ops[0].type == X.X86_OP_REG:
        if len(ops) != 2 or ops[0].type != X.X86_OP_REG:
            raise Unhandled('xmm move form')
        dst = xmm_idx(ops[0].reg)
        scalar = mn in ('movss','movsd')
        n = 4 if mn == 'movss' else 8 if mn == 'movsd' else 16
        if ops[1].type == X.X86_OP_REG:
            src = xmm_idx(ops[1].reg)
            if mn in ('movss','movsd'):
                S.append('jit_xmm_copy(c,%d,%d,%d);' % (dst,src,n))
            else: S.append('jit_xmm_copy(c,%d,%d,16);' % (dst,src))
        elif ops[1].type == X.X86_OP_MEM:
            S.append('jit_xmm_load(c,%d,%s,%d,%d);' % (dst,xmm_mem(ops[1]),n,1 if scalar else 0))
        else: raise Unhandled('xmm move source')
        return S, None
    if mn in ('movss','movsd','movups','movaps','movdqa','movdqu') and ops[0].type == X.X86_OP_MEM:
        if len(ops) != 2 or ops[1].type != X.X86_OP_REG: raise Unhandled('xmm store form')
        src=xmm_idx(ops[1].reg); n=4 if mn=='movss' else 8 if mn=='movsd' else 16
        S.append('jit_xmm_store(c,%d,%s,%d);' % (src,xmm_mem(ops[0]),n)); return S, None
    if mn == 'movntps':
        if len(ops) != 2 or ops[0].type != X.X86_OP_MEM or ops[1].type != X.X86_OP_REG:
            raise Unhandled('movntps form')
        S.append('jit_xmm_store16(c,%d,%s);' % (xmm_idx(ops[1].reg), xmm_mem(ops[0])))
        return S, None
    if mn in ('movsb','movsw','stosb','stosw','stosd') or (mn=='movsd' and len(ops)==0):
        size=1 if mn.endswith('b') else 2 if mn.endswith('w') else 4
        S.append('jit_string_one(c,%d,%d);' % (size,1 if mn.startswith('stos') else 0))
        return S, None
    if mn == 'vmovups':
        if len(ops) != 2 or ops[0].size not in (16,32) or ops[1].size != ops[0].size:
            raise Unhandled('vmovups non-128 form')
        if ops[0].size == 32:
            if ops[0].type == X.X86_OP_REG and ops[1].type == X.X86_OP_MEM: S.append('jit_ymm_load(c,%d,%s);' % (ymm_idx(ops[0].reg),xmm_mem(ops[1])))
            elif ops[0].type == X.X86_OP_MEM and ops[1].type == X.X86_OP_REG: S.append('jit_ymm_store(c,%d,%s);' % (ymm_idx(ops[1].reg),xmm_mem(ops[0])))
            else: raise Unhandled('vmovups ymm form')
            return S, None
        if ops[0].type == X.X86_OP_REG and ops[1].type == X.X86_OP_REG:
            S.append('jit_xmm_copy(c,%d,%d,16);' % (xmm_idx(ops[0].reg), xmm_idx(ops[1].reg)))
        elif ops[0].type == X.X86_OP_REG and ops[1].type == X.X86_OP_MEM:
            S.append('jit_xmm_load(c,%d,%s,16,0);' % (xmm_idx(ops[0].reg), xmm_mem(ops[1])))
        elif ops[0].type == X.X86_OP_MEM and ops[1].type == X.X86_OP_REG:
            S.append('jit_xmm_store(c,%d,%s,16);' % (xmm_idx(ops[1].reg), xmm_mem(ops[0])))
        else: raise Unhandled('vmovups operands')
        return S, None
    if mn in ('vaddps','vsubps','vmulps','vdivps'):
        if len(ops) != 3 or any(o.size not in (16,32) for o in ops) or any(o.size != ops[0].size for o in ops) or ops[0].type != X.X86_OP_REG or ops[1].type != X.X86_OP_REG:
            raise Unhandled('vpacked float form')
        op={'vaddps':0,'vsubps':1,'vmulps':2,'vdivps':3}[mn]
        if ops[0].size==32:
            dst=ymm_idx(ops[0].reg);src1=ymm_idx(ops[1].reg)
            if ops[2].type==X.X86_OP_REG:S.append('jit_ymm_binps3(c,%d,%d,%d,0,0,%d);' % (dst,src1,ymm_idx(ops[2].reg),op))
            elif ops[2].type==X.X86_OP_MEM:S.append('jit_ymm_binps3(c,%d,%d,0,%s,1,%d);' % (dst,src1,xmm_mem(ops[2]),op))
        else:
            dst=xmm_idx(ops[0].reg); src1=xmm_idx(ops[1].reg)
            if ops[2].type==X.X86_OP_REG: S.append('jit_xmm_binps3(c,%d,%d,%d,0,0,%d);' % (dst,src1,xmm_idx(ops[2].reg),op))
            elif ops[2].type==X.X86_OP_MEM: S.append('jit_xmm_binps3(c,%d,%d,0,%s,1,%d);' % (dst,src1,xmm_mem(ops[2]),op))
            else: raise Unhandled('vpacked float source')
        if ops[2].type not in (X.X86_OP_REG,X.X86_OP_MEM): raise Unhandled('vpacked float source')
        return S, None
    if mn == 'vshufps':
        if len(ops) != 4 or ops[0].size not in (16,32) or ops[0].type != X.X86_OP_REG or ops[1].type != X.X86_OP_REG or ops[3].type != X.X86_OP_IMM:
            raise Unhandled('vshufps form')
        if ops[0].size==32:
            d=ymm_idx(ops[0].reg);s=ymm_idx(ops[1].reg); fn='jit_ymm_shufps3'; si=ymm_idx(ops[2].reg) if ops[2].type==X.X86_OP_REG else 0
            if ops[2].type==X.X86_OP_REG:S.append('%s(c,%d,%d,%d,0,0,%du);' % (fn,d,s,si,ops[3].imm&0xff))
            elif ops[2].type==X.X86_OP_MEM:S.append('%s(c,%d,%d,0,%s,1,%du);' % (fn,d,s,xmm_mem(ops[2]),ops[3].imm&0xff))
            else: raise Unhandled('vshufps source')
        elif ops[2].type==X.X86_OP_REG: S.append('jit_xmm_shufps3(c,%d,%d,%d,0,0,%du);' % (xmm_idx(ops[0].reg),xmm_idx(ops[1].reg),xmm_idx(ops[2].reg),ops[3].imm&0xff))
        elif ops[2].type==X.X86_OP_MEM: S.append('jit_xmm_shufps3(c,%d,%d,0,%s,1,%du);' % (xmm_idx(ops[0].reg),xmm_idx(ops[1].reg),xmm_mem(ops[2]),ops[3].imm&0xff))
        else: raise Unhandled('vshufps source')
        return S, None
    if mn == 'vblendps':
        if len(ops) != 4 or ops[0].type != X.X86_OP_REG or ops[1].type != X.X86_OP_REG or ops[3].type != X.X86_OP_IMM or ops[0].size not in (16,32):
            raise Unhandled('vblendps form')
        width=ops[0].size
        if width==32:
            d=ymm_idx(ops[0].reg);s1=ymm_idx(ops[1].reg);s2=ymm_idx(ops[2].reg) if ops[2].type==X.X86_OP_REG else 0
        else:
            d=xmm_idx(ops[0].reg);s1=xmm_idx(ops[1].reg);s2=xmm_idx(ops[2].reg) if ops[2].type==X.X86_OP_REG else 0
        if ops[2].type==X.X86_OP_REG:S.append('jit_vec_blendps(c,%d,%d,%d,%d,0,0,%du);' % (width,d,s1,s2,ops[3].imm&0xff))
        elif ops[2].type==X.X86_OP_MEM:S.append('jit_vec_blendps(c,%d,%d,%d,0,%s,1,%du);' % (width,d,s1,xmm_mem(ops[2]),ops[3].imm&0xff))
        else: raise Unhandled('vblendps source')
        return S, None
    if mn == 'vperm2f128':
        if len(ops) != 4 or ops[0].type != X.X86_OP_REG or ops[1].type != X.X86_OP_REG or ops[2].type not in (X.X86_OP_REG,X.X86_OP_MEM) or ops[3].type != X.X86_OP_IMM or ops[0].size != 32:
            raise Unhandled('vperm2f128 form')
        if ops[2].type==X.X86_OP_REG:S.append('jit_ymm_perm2f128(c,%d,%d,%d,0,0,%du);' % (ymm_idx(ops[0].reg),ymm_idx(ops[1].reg),ymm_idx(ops[2].reg),ops[3].imm&0xff))
        elif ops[2].type==X.X86_OP_MEM:S.append('jit_ymm_perm2f128(c,%d,%d,0,%s,1,%du);' % (ymm_idx(ops[0].reg),ymm_idx(ops[1].reg),xmm_mem(ops[2]),ops[3].imm&0xff))
        else: raise Unhandled('vperm2f128 source')
        return S, None
    if mn == 'pinsrw':
        if len(ops)!=3 or ops[0].type!=X.X86_OP_REG or not md.reg_name(ops[0].reg).startswith('xmm') or ops[2].type!=X.X86_OP_IMM:
            raise Unhandled('pinsrw form')
        v,_=rd_op(insn,1); S.append('jit_xmm_pinsrw(c,%d,%s,%du);' % (xmm_idx(ops[0].reg),v,ops[2].imm&7)); return S,None
    if mn == 'pextrw':
        if (len(ops) != 3 or ops[0].type != X.X86_OP_REG or
                ops[1].type != X.X86_OP_REG or
                not md.reg_name(ops[1].reg).startswith('xmm') or
                ops[2].type != X.X86_OP_IMM):
            raise Unhandled('pextrw form')
        # pextrw always zero-extends the selected 16-bit lane into the
        # general-purpose destination.  Keep the byte copy unaligned-safe,
        # matching the interpreter's byte-exact implementation.
        S.append('{ uint16_t _v; memcpy(&_v,c->xmm[%d]+%du,2); write_reg(c,%d,4,_v); }' %
                 (xmm_idx(ops[1].reg), (ops[2].imm & 7) * 2, REG[ops[0].reg][0]))
        return S, None
    if mn == 'psadbw':
        if len(ops)!=2 or ops[0].type!=X.X86_OP_REG or not md.reg_name(ops[0].reg).startswith('xmm'):
            raise Unhandled('psadbw form')
        d=xmm_idx(ops[0].reg)
        if ops[1].type==X.X86_OP_REG:S.append('jit_xmm_psadbw(c,%d,%d,0,0);' % (d,xmm_idx(ops[1].reg)))
        elif ops[1].type==X.X86_OP_MEM:S.append('jit_xmm_psadbw(c,%d,0,%s,1);' % (d,xmm_mem(ops[1])))
        else: raise Unhandled('psadbw source')
        return S,None
    if mn == 'pmuludq':
        if len(ops)!=2 or ops[0].type!=X.X86_OP_REG or not md.reg_name(ops[0].reg).startswith('xmm'):
            raise Unhandled('pmuludq form')
        d=xmm_idx(ops[0].reg)
        if ops[1].type==X.X86_OP_REG:S.append('jit_xmm_pmuludq(c,%d,%d,0,0);' % (d,xmm_idx(ops[1].reg)))
        elif ops[1].type==X.X86_OP_MEM:S.append('jit_xmm_pmuludq(c,%d,0,%s,1);' % (d,xmm_mem(ops[1])))
        else: raise Unhandled('pmuludq source')
        return S,None
    if mn in ('xorps','xorpd','pxor'):
        if len(ops)!=2 or ops[0].type!=X.X86_OP_REG: raise Unhandled('xmm xor form')
        ismm=md.reg_name(ops[0].reg).startswith('mm'); dst=mm_idx(ops[0].reg) if ismm else xmm_idx(ops[0].reg)
        if ops[1].type==X.X86_OP_REG:
            src=mm_idx(ops[1].reg) if ismm else xmm_idx(ops[1].reg); S.append('jit_mmx_logic(c,%d,%d,0,0,2);' % (dst,src) if ismm else 'jit_xmm_xor(c,%d,%d);' % (dst,src))
        elif ops[1].type==X.X86_OP_MEM:S.append('jit_mmx_logic(c,%d,0,%s,1,2);' % (dst,xmm_mem(ops[1])) if ismm else 'jit_xmm_logic(c,%d,0,%s,1,3);' % (dst,xmm_mem(ops[1])))
        else: raise Unhandled('xmm xor source')
        return S, None
    if mn in ('unpcklps','unpckhps'):
        if len(ops)!=2 or ops[0].type!=X.X86_OP_REG: raise Unhandled('xmm unpack form')
        dst=xmm_idx(ops[0].reg); high=1 if mn=='unpckhps' else 0
        if ops[1].type==X.X86_OP_REG:S.append('jit_xmm_unpck_int(c,%d,%d,4,%d);' % (dst,xmm_idx(ops[1].reg),high))
        elif ops[1].type==X.X86_OP_MEM:S.append('jit_xmm_unpck_mem(c,%d,%s,4,%d);' % (dst,xmm_mem(ops[1]),high))
        else: raise Unhandled('xmm unpack source')
        return S, None
    if mn in ('movlhps','movhlps'):
        if len(ops)!=2 or ops[0].type!=X.X86_OP_REG or ops[1].type!=X.X86_OP_REG: raise Unhandled('xmm half form')
        S.append('jit_xmm_movehalf(c,%d,%d,%d);' % (xmm_idx(ops[0].reg),xmm_idx(ops[1].reg),1 if mn=='movlhps' else 0)); return S, None
    if mn in ('addss','subss','mulss','divss'):
        if len(ops)!=2 or ops[0].type!=X.X86_OP_REG: raise Unhandled('scalar sse form')
        dst=xmm_idx(ops[0].reg); op={'addss':0,'subss':1,'mulss':2,'divss':3}[mn]
        if ops[1].type==X.X86_OP_REG:
            S.append('jit_xmm_binss(c,%d,%d,0,0,%d);' % (dst,xmm_idx(ops[1].reg),op))
        elif ops[1].type==X.X86_OP_MEM:
            S.append('jit_xmm_binss(c,%d,0,%s,1,%d);' % (dst,xmm_mem(ops[1]),op))
        else: raise Unhandled('scalar sse source')
        return S, None
    if mn == 'rsqrtss':
        if len(ops)!=2 or ops[0].type!=X.X86_OP_REG: raise Unhandled('rsqrtss form')
        dst=xmm_idx(ops[0].reg)
        if ops[1].type==X.X86_OP_REG:S.append('jit_xmm_rsqrtss(c,%d,%d,0,0);' % (dst,xmm_idx(ops[1].reg)))
        elif ops[1].type==X.X86_OP_MEM:S.append('jit_xmm_rsqrtss(c,%d,0,%s,1);' % (dst,xmm_mem(ops[1])))
        else: raise Unhandled('rsqrtss source')
        return S,None
    if mn in ('minss','maxss'):
        if len(ops)!=2 or ops[0].type!=X.X86_OP_REG: raise Unhandled('minmax form')
        dst=xmm_idx(ops[0].reg); mx=1 if mn=='maxss' else 0
        if ops[1].type==X.X86_OP_REG:S.append('jit_xmm_minmaxss(c,%d,%d,0,0,%d);' % (dst,xmm_idx(ops[1].reg),mx))
        elif ops[1].type==X.X86_OP_MEM:S.append('jit_xmm_minmaxss(c,%d,0,%s,1,%d);' % (dst,xmm_mem(ops[1]),mx))
        else: raise Unhandled('minmax source')
        return S, None
    if mn in ('addps','subps','mulps','divps'):
        if len(ops)!=2 or ops[0].type!=X.X86_OP_REG: raise Unhandled('packed float form')
        dst=xmm_idx(ops[0].reg); op={'addps':0,'subps':1,'mulps':2,'divps':3}[mn]
        if ops[1].type==X.X86_OP_REG:S.append('jit_xmm_binps(c,%d,%d,0,0,%d);' % (dst,xmm_idx(ops[1].reg),op))
        elif ops[1].type==X.X86_OP_MEM:S.append('jit_xmm_binps(c,%d,0,%s,1,%d);' % (dst,xmm_mem(ops[1]),op))
        else: raise Unhandled('packed float source')
        return S, None
    if mn == 'sqrtss':
        if len(ops)!=2 or ops[0].type!=X.X86_OP_REG: raise Unhandled('sqrtss form')
        dst=xmm_idx(ops[0].reg)
        if ops[1].type==X.X86_OP_REG: S.append('{ float _s; memcpy(&_s,c->xmm[%d],4); _s=sqrtf(_s); memcpy(c->xmm[%d],&_s,4); }' % (xmm_idx(ops[1].reg),dst))
        else: raise Unhandled('sqrtss source')
        return S, None
    if mn == 'sqrtps':
        if len(ops)!=2 or ops[0].type!=X.X86_OP_REG or ops[1].type!=X.X86_OP_REG: raise Unhandled('sqrtps form')
        S.append('jit_xmm_sqrt(c,%d,%d);' % (xmm_idx(ops[0].reg),xmm_idx(ops[1].reg))); return S, None
    if mn == 'cvtsi2ss':
        if len(ops)!=2 or ops[0].type!=X.X86_OP_REG: raise Unhandled('cvtsi2ss form')
        dst=xmm_idx(ops[0].reg); v,_=rd_op(insn,1); S.append('jit_xmm_cvtsi2ss(c,%d,%s);' % (dst,v)); return S, None
    if mn in ('comiss','ucomiss'):
        if len(ops)!=2 or ops[0].type!=X.X86_OP_REG: raise Unhandled('comiss form')
        dst=xmm_idx(ops[0].reg)
        if ops[1].type==X.X86_OP_REG: S.append('jit_xmm_compare_ss(c,%d,%d,0,0);' % (dst,xmm_idx(ops[1].reg)))
        elif ops[1].type==X.X86_OP_MEM: S.append('jit_xmm_compare_ss(c,%d,0,%s,1);' % (dst,xmm_mem(ops[1])))
        else: raise Unhandled('comiss source')
        return S, None
    if mn in ('cmpeqss','cmpltss','cmpless','cmpunordss','cmpneqss','cmpnltss','cmpnless','cmpordss','cmpps','cmpeqps','cmpltps','cmpleps','cmpunordps','cmpneqps','cmpnltps','cmpnleps','cmpordps'):
        if len(ops)!=2 or ops[0].type!=X.X86_OP_REG: raise Unhandled('compare mask form')
        names=('cmpeq','cmplt','cmple','cmpunord','cmpneq','cmpnlt','cmpnle','cmpord');base=mn[:-2] if mn.endswith(('ss','ps')) else mn
        if base not in names: raise Unhandled('compare predicate')
        pred=names.index(base);dst=xmm_idx(ops[0].reg);scalar=1 if mn.endswith('ss') else 0
        if ops[1].type==X.X86_OP_REG:S.append('jit_xmm_cmpps(c,%d,%d,0,0,%d,%d);' % (dst,xmm_idx(ops[1].reg),pred,scalar))
        elif ops[1].type==X.X86_OP_MEM:S.append('jit_xmm_cmpps(c,%d,0,%s,1,%d,%d);' % (dst,xmm_mem(ops[1]),pred,scalar))
        else: raise Unhandled('compare mask source')
        return S,None
    if mn in ('cvttss2si','cvtss2si'):
        if len(ops)!=2 or ops[0].type!=X.X86_OP_REG: raise Unhandled('float convert form')
        idx,_=REG.get(ops[0].reg,(None,None));
        if idx is None: raise Unhandled('float convert destination')
        truncv=1 if mn=='cvttss2si' else 0
        if ops[1].type==X.X86_OP_REG: S.append('c->regs[%d]=(uint32_t)jit_xmm_cvtss2si(c,%d,0,0,%d);' % (idx,xmm_idx(ops[1].reg),truncv))
        elif ops[1].type==X.X86_OP_MEM: S.append('c->regs[%d]=(uint32_t)jit_xmm_cvtss2si(c,0,%s,1,%d);' % (idx,xmm_mem(ops[1]),truncv))
        else: raise Unhandled('float convert source')
        return S, None
    if mn in ('andps','andnps','orps','por','pand','pandn'):
        if len(ops)!=2 or ops[0].type!=X.X86_OP_REG: raise Unhandled('xmm logic form')
        ismm=md.reg_name(ops[0].reg).startswith('mm');dst=mm_idx(ops[0].reg) if ismm else xmm_idx(ops[0].reg); op=0 if mn in ('andps','pand') else 1 if mn in ('orps','por') else 2 if mn in ('andnps','pandn') else 3
        if ops[1].type==X.X86_OP_REG:S.append('jit_mmx_logic(c,%d,%d,0,0,%d);' % (dst,mm_idx(ops[1].reg),op) if ismm else 'jit_xmm_logic(c,%d,%d,0,0,%d);' % (dst,xmm_idx(ops[1].reg),op))
        elif ops[1].type==X.X86_OP_MEM:S.append('jit_mmx_logic(c,%d,0,%s,1,%d);' % (dst,xmm_mem(ops[1]),op) if ismm else 'jit_xmm_logic(c,%d,0,%s,1,%d);' % (dst,xmm_mem(ops[1]),op))
        else: raise Unhandled('xmm logic source')
        return S, None
    if mn in ('pmaxub','pminub'):
        if len(ops)!=2 or ops[0].type!=X.X86_OP_REG: raise Unhandled('minmax packed form')
        dst=xmm_idx(ops[0].reg); mx=1 if mn=='pmaxub' else 0
        if ops[1].type==X.X86_OP_REG:S.append('jit_xmm_minmaxub(c,%d,%d,0,0,%d);' % (dst,xmm_idx(ops[1].reg),mx))
        elif ops[1].type==X.X86_OP_MEM:S.append('jit_xmm_minmaxub(c,%d,0,%s,1,%d);' % (dst,xmm_mem(ops[1]),mx))
        else: raise Unhandled('minmax packed source')
        return S, None
    if mn == 'pmaddubsw':
        if len(ops)!=2 or ops[0].type!=X.X86_OP_REG: raise Unhandled('pmaddubsw form')
        dst=xmm_idx(ops[0].reg)
        if ops[1].type==X.X86_OP_REG:S.append('jit_xmm_pmaddubsw(c,%d,%d,0,0);' % (dst,xmm_idx(ops[1].reg)))
        elif ops[1].type==X.X86_OP_MEM:S.append('jit_xmm_pmaddubsw(c,%d,0,%s,1);' % (dst,xmm_mem(ops[1])))
        else: raise Unhandled('pmaddubsw source')
        return S, None
    if mn in ('paddb','paddw','paddd','psubb','psubw','psubd'):
        if len(ops)!=2 or ops[0].type!=X.X86_OP_REG: raise Unhandled('packed add form')
        ismm=md.reg_name(ops[0].reg).startswith('mm');dst=mm_idx(ops[0].reg) if ismm else xmm_idx(ops[0].reg); width=1 if mn[-1]=='b' else 2 if mn[-1]=='w' else 4; sub=1 if mn.startswith('psub') else 0
        if ismm:
            if ops[1].type==X.X86_OP_REG:S.append('jit_mmx_addsub(c,%d,%d,0,0,%d,%d);' % (dst,mm_idx(ops[1].reg),width,sub))
            elif ops[1].type==X.X86_OP_MEM:S.append('jit_mmx_addsub(c,%d,0,%s,1,%d,%d);' % (dst,xmm_mem(ops[1]),width,sub))
            else: raise Unhandled('mmx add source')
            return S, None
        if ops[1].type==X.X86_OP_REG: S.append('jit_xmm_packed_addsub(c,%d,%d,0,0,%d,%d);' % (dst,xmm_idx(ops[1].reg),width,sub))
        elif ops[1].type==X.X86_OP_MEM: S.append('jit_xmm_packed_addsub(c,%d,0,%s,1,%d,%d);' % (dst,xmm_mem(ops[1]),width,sub))
        else: raise Unhandled('packed add source')
        return S, None
    if mn in ('punpcklbw','punpcklwd','punpckldq','punpcklqdq','punpckhbw','punpckhwd','punpckhdq','punpckhqdq'):
        if len(ops)!=2 or ops[0].type!=X.X86_OP_REG: raise Unhandled('packed unpack form')
        width=1 if mn.endswith('bw') else 2 if mn.endswith('wd') else 8 if mn.endswith('qdq') else 4
        high=1 if mn.startswith('punpckh') else 0; ismm=md.reg_name(ops[0].reg).startswith('mm'); dst=mm_idx(ops[0].reg) if ismm else xmm_idx(ops[0].reg)
        if ismm:
            if ops[1].type==X.X86_OP_REG:S.append('jit_mmx_unpck(c,%d,%d,0,0,%d,%d);' % (dst,mm_idx(ops[1].reg),width,high))
            elif ops[1].type==X.X86_OP_MEM:S.append('jit_mmx_unpck(c,%d,0,%s,1,%d,%d);' % (dst,xmm_mem(ops[1]),width,high))
            else: raise Unhandled('mmx unpack source')
            return S,None
        if ops[1].type==X.X86_OP_REG:S.append('jit_xmm_unpck_int(c,%d,%d,%d,%d);' % (dst,xmm_idx(ops[1].reg),width,high))
        elif ops[1].type==X.X86_OP_MEM:S.append('jit_xmm_unpck_mem(c,%d,%s,%d,%d);' % (dst,xmm_mem(ops[1]),width,high))
        else: raise Unhandled('packed unpack source')
        return S, None
    if mn in ('paddsb','paddsw','psubsb','psubsw','paddusb','paddusw','psubusb','psubusw') and not (ops and ops[0].type==X.X86_OP_REG and md.reg_name(ops[0].reg).startswith('mm')):
        if len(ops)!=2 or ops[0].type!=X.X86_OP_REG: raise Unhandled('saturating packed form')
        dst=xmm_idx(ops[0].reg); width=1 if mn.endswith('b') else 2; sub=1 if mn.startswith('psub') else 0; signedv=0 if 'usb' in mn or 'usw' in mn else 1
        if ops[1].type==X.X86_OP_REG: S.append('jit_xmm_sat_addsub(c,%d,%d,0,0,%d,%d,%d);' % (dst,xmm_idx(ops[1].reg),width,sub,signedv))
        elif ops[1].type==X.X86_OP_MEM: S.append('jit_xmm_sat_addsub(c,%d,0,%s,1,%d,%d,%d);' % (dst,xmm_mem(ops[1]),width,sub,signedv))
        else: raise Unhandled('saturating packed source')
        return S, None
    if mn in ('pshufd','pshuflw','pshufhw','shufps'):
        if len(ops)<3 or ops[0].type!=X.X86_OP_REG: raise Unhandled('shuffle form')
        dst=xmm_idx(ops[0].reg); si=2
        if ops[si].type!=X.X86_OP_IMM: raise Unhandled('shuffle immediate')
        imm=ops[si].imm&0xff
        srcop=ops[1]
        mode={'pshufd':0,'pshuflw':1,'pshufhw':2,'shufps':3}[mn]
        if srcop.type==X.X86_OP_REG: S.append('jit_xmm_shuffle(c,%d,%d,0,0,%du,%d);' % (dst,xmm_idx(srcop.reg),imm,mode))
        elif srcop.type==X.X86_OP_MEM: S.append('jit_xmm_shuffle(c,%d,0,%s,1,%du,%d);' % (dst,xmm_mem(srcop),imm,mode))
        else: raise Unhandled('shuffle source')
        return S, None
    if mn in ('pmullw','pmaddwd'):
        if len(ops)!=2 or ops[0].type!=X.X86_OP_REG: raise Unhandled('packed multiply form')
        if mn=='pmullw' and md.reg_name(ops[0].reg).startswith('mm'):
            dst=mm_idx(ops[0].reg)
            if ops[1].type==X.X86_OP_REG:S.append('jit_mmx_pmullw(c,%d,%d,0,0);' % (dst,mm_idx(ops[1].reg)))
            elif ops[1].type==X.X86_OP_MEM:S.append('jit_mmx_pmullw(c,%d,0,%s,1);' % (dst,xmm_mem(ops[1])))
            else: raise Unhandled('mmx multiply source')
            return S, None
        if mn=='pmaddwd' and md.reg_name(ops[0].reg).startswith('mm'): raise Unhandled('mmx pmaddwd form')
        dst=xmm_idx(ops[0].reg); fn='jit_xmm_pmullw' if mn=='pmullw' else 'jit_xmm_pmaddwd'
        if ops[1].type==X.X86_OP_REG: S.append('%s(c,%d,%d,0,0);' % (fn,dst,xmm_idx(ops[1].reg)))
        elif ops[1].type==X.X86_OP_MEM: S.append('%s(c,%d,0,%s,1);' % (fn,dst,xmm_mem(ops[1])))
        else: raise Unhandled('packed multiply source')
        return S, None
    if mn == 'pmulhw':
        if len(ops)!=2 or ops[0].type!=X.X86_OP_REG: raise Unhandled('pmulhw form')
        ismm=md.reg_name(ops[0].reg).startswith('mm');dst=mm_idx(ops[0].reg) if ismm else xmm_idx(ops[0].reg);fn='jit_mmx_pmulhw' if ismm else 'jit_xmm_pmulhw'
        if ops[1].type==X.X86_OP_REG:S.append('%s(c,%d,%d,0,0);' % (fn,dst,mm_idx(ops[1].reg) if ismm else xmm_idx(ops[1].reg)))
        elif ops[1].type==X.X86_OP_MEM:S.append('%s(c,%d,0,%s,1);' % (fn,dst,xmm_mem(ops[1])))
        else: raise Unhandled('pmulhw source')
        return S, None
    if mn in ('paddsw','psubsw') and md.reg_name(ops[0].reg).startswith('mm'):
        if len(ops)!=2: raise Unhandled('mmx sat word form')
        dst=mm_idx(ops[0].reg);sub=1 if mn=='psubsw' else 0
        if ops[1].type==X.X86_OP_REG:S.append('jit_mmx_satw(c,%d,%d,0,0,%d);' % (dst,mm_idx(ops[1].reg),sub))
        elif ops[1].type==X.X86_OP_MEM:S.append('jit_mmx_satw(c,%d,0,%s,1,%d);' % (dst,xmm_mem(ops[1]),sub))
        else: raise Unhandled('mmx sat word source')
        return S,None
    if mn in ('psraw','psrad','psrlw','psrld','psrlq','psllw','pslld','psllq'):
        if len(ops)!=2 or ops[0].type!=X.X86_OP_REG: raise Unhandled('packed shift form')
        ismm=md.reg_name(ops[0].reg).startswith('mm');dst=mm_idx(ops[0].reg) if ismm else xmm_idx(ops[0].reg); width=2 if mn.endswith('w') else 8 if mn.endswith('q') else 4
        arith=1 if mn.startswith('psra') else 0; left=1 if mn.startswith('psll') else 0
        if ops[1].type==X.X86_OP_IMM: count='%du' % (ops[1].imm & 0xff)
        elif ops[1].type==X.X86_OP_REG: count=('jit_mmx_count(c,%d)' % mm_idx(ops[1].reg)) if ismm else ('jit_xmm_count(c,%d)' % xmm_idx(ops[1].reg))
        else: raise Unhandled('packed shift count')
        S.append(('jit_mmx_shift' if ismm else 'jit_xmm_shift')+'(c,%d,%s,%d,%d,%d);' % (dst,count,width,arith,left)); return S, None
    if mn in ('packuswb','packssdw','packsswb') and not (ops and ops[0].type==X.X86_OP_REG and md.reg_name(ops[0].reg).startswith('mm')):
        if len(ops)!=2 or ops[0].type!=X.X86_OP_REG: raise Unhandled('pack form')
        dst=xmm_idx(ops[0].reg); tw=1 if mn=='packssdw' else 0; uv=1 if mn=='packuswb' else 0
        if ops[1].type==X.X86_OP_REG: S.append('jit_xmm_pack(c,%d,%d,0,0,%d,%d);' % (dst,xmm_idx(ops[1].reg),tw,uv))
        elif ops[1].type==X.X86_OP_MEM: S.append('jit_xmm_pack(c,%d,0,%s,1,%d,%d);' % (dst,xmm_mem(ops[1]),tw,uv))
        else: raise Unhandled('pack source')
        return S, None
    if mn in ('packsswb','packuswb') and md.reg_name(ops[0].reg).startswith('mm'):
        if len(ops)!=2: raise Unhandled('mmx pack form')
        dst=mm_idx(ops[0].reg);uns=1 if mn=='packuswb' else 0
        if ops[1].type==X.X86_OP_REG:S.append('jit_mmx_pack(c,%d,%d,0,0,0,%d);' % (dst,mm_idx(ops[1].reg),uns))
        elif ops[1].type==X.X86_OP_MEM:S.append('jit_mmx_pack(c,%d,0,%s,1,0,%d);' % (dst,xmm_mem(ops[1]),uns))
        else: raise Unhandled('mmx pack source')
        return S, None
    if mn in ('pavgb','pavgw'):
        if len(ops)!=2 or ops[0].type!=X.X86_OP_REG: raise Unhandled('average form')
        dst=xmm_idx(ops[0].reg); width=1 if mn=='pavgb' else 2
        if ops[1].type==X.X86_OP_REG:S.append('jit_xmm_avg(c,%d,%d,0,0,%d);' % (dst,xmm_idx(ops[1].reg),width))
        elif ops[1].type==X.X86_OP_MEM:S.append('jit_xmm_avg(c,%d,0,%s,1,%d);' % (dst,xmm_mem(ops[1]),width))
        else: raise Unhandled('average source')
        return S, None
    if mn in ('pcmpgtb','pcmpgtw','pcmpgtd') and not (ops and ops[0].type==X.X86_OP_REG and md.reg_name(ops[0].reg).startswith('mm')):
        if len(ops)!=2 or ops[0].type!=X.X86_OP_REG: raise Unhandled('compare form')
        dst=xmm_idx(ops[0].reg); width=1 if mn.endswith('b') else 2 if mn.endswith('w') else 4
        if ops[1].type==X.X86_OP_REG:S.append('jit_xmm_cmpgt(c,%d,%d,0,0,%d);' % (dst,xmm_idx(ops[1].reg),width))
        elif ops[1].type==X.X86_OP_MEM:S.append('jit_xmm_cmpgt(c,%d,0,%s,1,%d);' % (dst,xmm_mem(ops[1]),width))
        else: raise Unhandled('compare source')
        return S, None
    if mn in ('pcmpeqb','pcmpeqw','pcmpeqd','pcmpgtb','pcmpgtw','pcmpgtd') and not (ops and ops[0].type==X.X86_OP_REG and md.reg_name(ops[0].reg).startswith('mm')):
        if len(ops)!=2 or ops[0].type!=X.X86_OP_REG: raise Unhandled('compare equality form')
        dst=xmm_idx(ops[0].reg);width=1 if mn.endswith('b') else 2 if mn.endswith('w') else 4;gt=1 if mn.startswith('pcmpgt') else 0
        fn='jit_xmm_cmpgt' if gt else 'jit_xmm_cmpeq'
        if ops[1].type==X.X86_OP_REG:S.append('%s(c,%d,%d,0,0,%d);' % (fn,dst,xmm_idx(ops[1].reg),width))
        elif ops[1].type==X.X86_OP_MEM:S.append('%s(c,%d,0,%s,1,%d);' % (fn,dst,xmm_mem(ops[1]),width))
        else: raise Unhandled('compare equality source')
        return S, None
    if mn in ('pcmpeqb','pcmpeqw','pcmpeqd','pcmpgtb','pcmpgtw','pcmpgtd') and md.reg_name(ops[0].reg).startswith('mm'):
        if len(ops)!=2: raise Unhandled('mmx compare form')
        dst=mm_idx(ops[0].reg);width=1 if mn.endswith('b') else 2 if mn.endswith('w') else 4;gt=1 if mn.startswith('pcmpgt') else 0
        if ops[1].type==X.X86_OP_REG:S.append('jit_mmx_cmp(c,%d,%d,0,0,%d,%d);' % (dst,mm_idx(ops[1].reg),width,gt))
        elif ops[1].type==X.X86_OP_MEM:S.append('jit_mmx_cmp(c,%d,0,%s,1,%d,%d);' % (dst,xmm_mem(ops[1]),width,gt))
        else: raise Unhandled('mmx compare source')
        return S, None
    if mn in ('psrldq','pslldq'):
        if len(ops)!=2 or ops[0].type!=X.X86_OP_REG or ops[1].type!=X.X86_OP_IMM: raise Unhandled('byte shift form')
        S.append('jit_xmm_shift_bytes(c,%d,%du,%d);' % (xmm_idx(ops[0].reg),ops[1].imm&0xff,1 if mn=='pslldq' else 0)); return S, None
    if mn in ('rep stosb','rep stosw','rep stosd'):
        if mn.endswith('b'): sz=1
        elif mn.endswith('w'): sz=2
        else: sz=4
        S.append('jit_rep_stos(c,%d);' % sz); return S, None
    if mn in ('rep movsb','rep movsw','rep movsd'):
        if mn.endswith('b'): sz=1
        elif mn.endswith('w'): sz=2
        else: sz=4
        S.append('jit_rep_movs(c,%d);' % sz); return S, None
    if I == X.X86_INS_PUSH:
        v, _ = rd_op(insn, 0); S.append('push32(c, %s);' % v); return S, None
    if I == X.X86_INS_POP:
        S.append(wr_op(insn, 0, 'pop32(c)')); return S, None
    if I == X.X86_INS_LEAVE:
        S.append('c->regs[4]=c->regs[5]; c->regs[5]=pop32(c);'); return S, None
    if I == X.X86_INS_CDQ:
        S.append('c->regs[2]=(uint32_t)((int32_t)c->regs[0]>>31);'); return S, None
    # ---- control flow (ends the block).  The exit descriptor names the
    # statically-known successor VA(s) so the emit phase can resolve them to
    # g_genblk indices and the block can return one directly (chaining). ----
    if I == X.X86_INS_JMP:
        if ops[0].type == X.X86_OP_IMM:
            return S, ('jmp', ops[0].imm & 0xffffffff, None)
        v,_ = rd_op(insn, 0); return S, ('jmp', None, v)
    if I in CC:
        return S, ('cond', CC[I], ops[0].imm & 0xffffffff, (insn.address + insn.size) & 0xffffffff)
    if I == X.X86_INS_CALL:
        ret = (insn.address + insn.size) & 0xffffffff
        if ops[0].type == X.X86_OP_IMM:
            return S, ('call', ops[0].imm & 0xffffffff, ret, None)
        v,_ = rd_op(insn, 0); return S, ('call', None, ret, v)
    if I == X.X86_INS_RET:
        pb = (ops[0].imm & 0xffff) if (ops and ops[0].type == X.X86_OP_IMM) else 0
        return S, ('ret', pb)
    raise Unhandled('insn %s' % mn)

# --- disassemble the ranges, find block leaders, translate -------------------
insns = {}       # va -> capstone insn
order = []
if CFG_EXPORTS:
    # DLL text is not safe to linear-sweep: compiler/linker padding and some
    # Wine helper tables are embedded between functions.  Walk only from real
    # exported entries, following direct CFG edges and calls.  Unknown indirect
    # transfers remain interpreter fallbacks.
    pending = list(export_entries())
    seen = set()
    while pending:
        va = pending.pop()
        if va in seen: continue
        seen.add(va)
        while any(lo <= va < hi for lo, hi in RANGES) and va not in insns:
            one = list(md.disasm(va_bytes(va, 16), va, count=1))
            if not one: break
            insn = one[0]; insns[va] = insn; order.append(va)
            I = insn.id
            if I in CC or (I in (X.X86_INS_JMP, X.X86_INS_CALL) and
                            insn.operands and insn.operands[0].type == X.X86_OP_IMM):
                target = insn.operands[0].imm & 0xffffffff
                if any(lo <= target < hi for lo, hi in RANGES): pending.append(target)
            if I in CC: pending.append(va + insn.size)
            if I == X.X86_INS_RET or I == X.X86_INS_JMP or I == X.X86_INS_INT3:
                break
            va += insn.size
else:
    for lo, hi in RANGES:
        va = lo
        code = va_bytes(lo, hi - lo)
        for insn in md.disasm(code, lo):
            if insn.address >= hi: break
            insns[insn.address] = insn; order.append(insn.address)

# An explicitly proven entry may sit behind an undecodable data island in the
# linear stream.  Decode only those named entries afresh; this keeps the
# normal scan conservative while allowing a profiler-confirmed indirect
# target to become a real block leader.
for va in EXTRA_ENTRIES:
    if va in insns: continue
    for insn in md.disasm(va_bytes(va, 0x400), va):
        if insn.address >= va + 0x400: break
        insns[insn.address] = insn

# A few hot engine dispatchers use compiler-generated absolute jump tables:
#
#     jmp dword ptr [index*4 + table]
#
# Their destinations are real basic-block leaders, but they are invisible to
# the direct-branch leader scan below.  Discover only bounded, absolute 32-bit
# tables whose entries point back into one of the translated ranges.  This is
# deliberately conservative: an unfamiliar indirect jump remains an ordinary
# dynamic exit and the interpreter is still the correctness fallback.
indirect_targets = set()
import_thunks = set()
range_bounds = [(lo, hi) for lo, hi in RANGES]
for insn in insns.values():
    # PE import thunks are two-byte absolute indirect jumps.  They are often
    # reached only through an IAT call, so no direct branch makes their entry
    # point a leader.  Translating the one instruction still pays off even
    # though the destination must remain dynamic.
    if (insn.id == X.X86_INS_JMP and bytes(insn.bytes[:2]) == b'\xff\x25'
            and any(lo <= insn.address < hi for lo, hi in range_bounds)):
        import_thunks.add(insn.address)
    if insn.id != X.X86_INS_JMP or not insn.operands:
        continue
    op = insn.operands[0]
    if op.type != X.X86_OP_MEM:
        continue
    m = op.mem
    if m.base != 0 or m.index == 0 or m.scale != 4 or m.disp == 0:
        continue
    table = m.disp & 0xffffffff
    found = []
    for n in range(512):
        raw = va_bytes(table + n * 4, 4)
        if len(raw) != 4:
            break
        target = int.from_bytes(raw, 'little')
        if any(lo <= target < hi for lo, hi in range_bounds) and target in insns:
            found.append(target)
    # Requiring multiple in-range entries filters accidental indexed loads and
    # keeps isolated data from becoming a spurious translated entry point.
    if len(found) >= 2:
        indirect_targets.update(found)

# Direct calls through an absolute function-pointer slot are another common
# form of compiler-generated dispatch.  Unlike an indexed jump table, the
# call has no immediate code target, but the slot itself is unambiguous: only
# accept it when its current PE value points into one of our ranges and is an
# already decoded instruction boundary.  Imports and ordinary data pointers
# therefore remain dynamic exits.
for insn in insns.values():
    if insn.id != X.X86_INS_CALL or not insn.operands:
        continue
    op = insn.operands[0]
    if op.type != X.X86_OP_MEM or op.mem.base != 0 or op.mem.index != 0:
        continue
    slot = op.mem.disp & 0xffffffff
    raw = va_bytes(slot, 4)
    if len(raw) != 4:
        continue
    target = int.from_bytes(raw, 'little')
    if any(lo <= target < hi for lo, hi in range_bounds) and target in insns:
        indirect_targets.add(target)

# Also recognize the adjacent register form emitted when a compiler needs to
# materialize a function-pointer slot before calling it:
#
#     mov  reg, dword ptr [absolute_slot]
#     call reg
#
# The adjacency requirement is intentional: it prevents an unrelated later
# call from being associated with a mutable data load.  A constant materialized
# with `mov reg, imm32` is equally unambiguous.
ordered = sorted(insns)
for pos, va in enumerate(ordered[:-1]):
    mov = insns[va]
    nxt = insns[ordered[pos + 1]]
    if mov.id != X.X86_INS_MOV or len(mov.operands) != 2:
        continue
    dst, src = mov.operands
    if dst.type != X.X86_OP_REG or nxt.id != X.X86_INS_CALL:
        continue
    cop = nxt.operands[0] if nxt.operands else None
    if cop is None or cop.type != X.X86_OP_REG or cop.reg != dst.reg:
        continue
    target = 0
    if src.type == X.X86_OP_MEM and src.mem.base == 0 and src.mem.index == 0:
        raw = va_bytes(src.mem.disp & 0xffffffff, 4)
        if len(raw) == 4: target = int.from_bytes(raw, 'little')
    elif src.type == X.X86_OP_IMM:
        target = src.imm & 0xffffffff
    if target and any(lo <= target < hi for lo, hi in range_bounds) and target in insns:
        indirect_targets.add(target)

leaders = set(HOOKS)   # hooks are always block boundaries
for lo, hi in RANGES: leaders.add(lo)
leaders.update(EXTRA_ENTRIES)
leaders.update(indirect_targets)
leaders.update(import_thunks)
for va, insn in insns.items():
    I = insn.id
    if I in CC:                    # target + fall-through are leaders
        leaders.add(insn.operands[0].imm & 0xffffffff); leaders.add(va + insn.size)
    elif I == X.X86_INS_JMP and insn.operands[0].type == X.X86_OP_IMM:
        leaders.add(insn.operands[0].imm & 0xffffffff)
    elif I == X.X86_INS_CALL:
        leaders.add(va + insn.size)   # return address is a leader

# Optimised i386 builds commonly dispatch small helpers through a function
# pointer table.  Their entry points have no direct call edge for the static
# scan to follow.  A conservative extra signal is a plausible function
# prologue immediately after a RET: a RET ends the preceding function, while
# padding/data are filtered by requiring a standard callee-save prologue.  This
# recovers entries such as the hot helper at 0x008012e1 without treating every
# instruction boundary as a block (which would explode the generated module).
for va, insn in insns.items():
    if insn.id != X.X86_INS_RET:
        continue
    nxt = va + insn.size
    ni = insns.get(nxt)
    if ni is None:
        continue
    nb = bytes(ni.bytes)
    # Capstone returns one instruction per entry, so multi-byte prologues
    # such as push edi / push esi / push ebx must be checked across entries.
    n2 = insns.get(nxt + ni.size)
    n3 = insns.get(n2.address + n2.size) if n2 else None
    callee_pushes = (ni.mnemonic == 'push' and n2 and n3 and
                     n2.mnemonic == 'push' and n3.mnemonic == 'push')
    if (nb[:3] in (b'\x55\x89\xe5', b'\x55\x8b\xec') or callee_pushes or
            (ni.mnemonic == 'push' and n2 and n2.mnemonic == 'sub')):
        leaders.add(nxt)

# The same prologue can itself be the target of an indirect call and can be
# preceded by alignment bytes rather than a RET.  Add only the unmistakable
# push-based forms; this is still far smaller than making every instruction a
# possible leader.  Raw-byte recovery of entries is deliberately deferred
# until each candidate has an independent call-site proof.
for va, insn in insns.items():
    if insn.mnemonic != 'push':
        continue
    n2 = insns.get(va + insn.size)
    n3 = insns.get(n2.address + n2.size) if n2 else None
    three_pushes = n2 and n3 and n2.mnemonic == 'push' and n3.mnemonic == 'push'
    push_ebp_frame = (md.reg_name(insn.operands[0].reg) == 'ebp' if insn.operands and
                      insn.operands[0].type == X.X86_OP_REG else False)
    if three_pushes or (push_ebp_frame and n2 and n2.mnemonic == 'mov'):
        leaders.add(va)
leaders = sorted(l for l in leaders if l in insns)

leaderset = set(leaders)
blocks = {}      # leader va -> (lines, exit_desc, end_va, ninsn); absent if untranslatable
import collections
skipmn = collections.Counter()
for li, lead in enumerate(leaders):
    if lead in HOOKS: continue        # hooked addresses are never JIT-dispatched
    va = lead; lines = []; ok = True; ninsn = 0; exitd = None
    while True:
        if va not in insns:                       # ran off the disassembled range
            exitd = ('jmp', va, None); break      # fall-through to the next VA
        insn = insns[va]
        try:
            stmts, ctrl = translate_insn(insn)
        except Unhandled:
            # Not implemented in C: single-step it through the interpreter and
            # keep going.  jit_step1 returns 0 if it did not fall through to the
            # next VA (it branched/trapped), and then we leave the block.  This
            # is why no opcode can break a block any more.
            skipmn[insn.mnemonic] += 1
            nva = va + insn.size
            lines.append('    if (!jit_step1(c, %du + (uint32_t)%s, %du + (uint32_t)%s)) return -1;' % (va, SLIDE_SYMBOL, nva, SLIDE_SYMBOL))
            ninsn += 1
            if nva in leaderset:
                exitd = ('jmp', nva, None); va = nva; break
            va = nva
            continue
        for s in stmts: lines.append('    ' + s)
        ninsn += 1
        nva = va + insn.size
        if ctrl is not None:                                # control transfer ends the block
            exitd = ctrl; va = nva; break
        if nva in leaderset:                                # next insn starts a new block
            exitd = ('jmp', nva, None); va = nva; break     # straight-line into it
        va = nva
    if ok: blocks[lead] = (lines, exitd, va, ninsn)

# --- emit ---------------------------------------------------------------------
# Block index == position in the sorted g_genblk[] array.  Each block returns the
# g_genblk index of its statically-known successor (or -1 for dynamic/untranslated
# targets), so the driver chains straight to it with no per-block hash lookup.
sb = sorted(blocks)
idx_of = {lead: i for i, lead in enumerate(sb)}
def gidx(va): return idx_of.get(va, -1)
def emit_exit(exitd):
    t = exitd[0]
    if t == 'jmp':
        _, tgt, dyn = exitd
        if tgt is not None:
            return ['    c->eip = %du + (uint32_t)%s; return %d;' % (tgt, SLIDE_SYMBOL, gidx(tgt))]
        return ['    c->eip = %s; return -1;' % dyn]
    if t == 'cond':
        _, cc, tgt, fall = exitd
        return ['    if (cond(c,%d)) { c->eip = %du + (uint32_t)%s; return %d; }' % (cc, tgt, SLIDE_SYMBOL, gidx(tgt)),
                '    c->eip = %du + (uint32_t)%s; return %d;' % (fall, SLIDE_SYMBOL, gidx(fall))]
    if t == 'call':
        _, tgt, ret, dyn = exitd
        if tgt is not None:
            return ['    push32(c, %du + (uint32_t)%s); c->eip = %du + (uint32_t)%s; return %d;'
                    % (ret, SLIDE_SYMBOL, tgt, SLIDE_SYMBOL, gidx(tgt))]
        return ['    push32(c, %du + (uint32_t)%s); c->eip = %s; return -1;' % (ret, SLIDE_SYMBOL, dyn)]
    # ret
    _, pb = exitd
    if pb: return ['    { uint32_t r=pop32(c); c->regs[4]+=%du; c->eip=r; return -1; }' % pb]
    return ['    c->eip = pop32(c); return -1;']

o = open(OUT, 'w')
o.write('/* auto-generated by x86toc.py - do not edit */\n')
o.write('#ifdef WEBWINE_GENBLOCKS\n')
for lead in sb:
    lines, exitd, end, ninsn = blocks[lead]
    o.write('static int %sgblk_%08x(struct x86cpu *c){\n' % (GEN_PREFIX, lead))
    for ln in lines: o.write(ln + '\n')
    for ln in emit_exit(exitd): o.write(ln + '\n')
    o.write('}\n')
o.write('struct %sgenblk { uint32_t va, end; uint16_t ninsn; int (*fn)(struct x86cpu*); };\n' % GEN_PREFIX)
o.write('static const struct %sgenblk %sg_genblk[] = {\n' % (GEN_PREFIX, GEN_PREFIX))
for lead in sb:
    lines, exitd, end, ninsn = blocks[lead]
    o.write('  { 0x%08xu, 0x%08xu, %d, %sgblk_%08x },\n' % (lead, end, min(ninsn,65535), GEN_PREFIX, lead))
o.write('  { 0, 0, 0, 0 }\n};\n')
o.write('#endif\n')
o.close()
print('translated %d/%d basic blocks' % (len(blocks), len(leaders)))
print('top unhandled opcodes (block-killers):', file=sys.stderr)
for mn, n in skipmn.most_common(30): print('  %-8s %d' % (mn, n), file=sys.stderr)
