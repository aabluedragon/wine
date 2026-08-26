#!/usr/bin/env python3
# Static x86 -> C binary translator for the webwine interpreter.
#
# Disassembles ranges of netduke32.exe's .text with capstone, splits each into
# basic blocks, and emits C block functions that reuse the interpreter's own
# helpers (c->regs[], rd8/wr8, set_lazy, cond, push32/pop32, read_reg/write_reg).
# The output is #included into wasm_x86.c so the blocks make DIRECT calls to the
# helpers (no cross-module boundary) - this is the whole point vs a runtime JIT.
#
# Any instruction the translator doesn't handle makes its whole basic block
# untranslatable (skipped) so the interpreter transparently runs it: correctness
# is preserved and coverage grows as opcodes are added.
#
#   python3 x86toc.py netduke32.exe out.c  0x506c30-0x507000  0x408000-0x409000 ...
import sys, capstone
from capstone import x86 as X

EXE, OUT = sys.argv[1], sys.argv[2]
RANGES = []
HOOKS = set()   # native-hooked guest addresses: must be block boundaries and are
                # never themselves translated, so run()'s nat check always fires.
for a in sys.argv[3:]:
    if a.startswith('--hooks='):
        for h in a[len('--hooks='):].split(','):
            if h: HOOKS.add(int(h, 16))
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

def va_bytes(va, n):
    for name, sva, vsz, raw, rawsz in sections:
        if sva <= va < sva + vsz:
            off = raw + (va - sva)
            return data[off:off+n]
    return b''

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.detail = True

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
    if m.disp != 0 or not parts:
        parts.append('%du' % (m.disp & 0xffffffff))
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
        return '%du' % (op.imm & 0xffffffff), op.size
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
    if I == X.X86_INS_NOP: return [], None
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
    if I in (X.X86_INS_BSF, X.X86_INS_BSR):
        av, _ = rd_op(insn, 1)
        idx = '__builtin_ctz(a)' if I == X.X86_INS_BSF else '(31 - __builtin_clz(a))'
        S.append('{ uint32_t a=%s; if(a){ %s c->eflags&=~ZF; } else c->eflags|=ZF; c->lf_size=0; }'
                 % (av, wr_op(insn, 0, idx))); return S, None
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
for lo, hi in RANGES:
    va = lo
    code = va_bytes(lo, hi - lo)
    for insn in md.disasm(code, lo):
        if insn.address >= hi: break
        insns[insn.address] = insn; order.append(insn.address)

leaders = set(HOOKS)   # hooks are always block boundaries
for lo, hi in RANGES: leaders.add(lo)
for va, insn in insns.items():
    I = insn.id
    if I in CC:                    # target + fall-through are leaders
        leaders.add(insn.operands[0].imm & 0xffffffff); leaders.add(va + insn.size)
    elif I == X.X86_INS_JMP and insn.operands[0].type == X.X86_OP_IMM:
        leaders.add(insn.operands[0].imm & 0xffffffff)
    elif I == X.X86_INS_CALL:
        leaders.add(va + insn.size)   # return address is a leader
# Split around every UNTRANSLATABLE instruction so it alone falls to the
# interpreter instead of killing its whole basic block.  A single float/SSE op
# used to discard all the surrounding integer instructions (the histogram showed
# millions of already-handled mov/add/test stuck in the interpreter); making the
# op and its successor both leaders recovers the integer prefix AND suffix.
for va, insn in insns.items():
    try:
        translate_insn(insn)
    except Unhandled:
        leaders.add(va); leaders.add(va + insn.size)
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
            ok = False; skipmn[insn.mnemonic] += 1; break
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
            return ['    c->eip = %du + (uint32_t)nd_slide; return %d;' % (tgt, gidx(tgt))]
        return ['    c->eip = %s; return -1;' % dyn]
    if t == 'cond':
        _, cc, tgt, fall = exitd
        return ['    if (cond(c,%d)) { c->eip = %du + (uint32_t)nd_slide; return %d; }' % (cc, tgt, gidx(tgt)),
                '    c->eip = %du + (uint32_t)nd_slide; return %d;' % (fall, gidx(fall))]
    if t == 'call':
        _, tgt, ret, dyn = exitd
        if tgt is not None:
            return ['    push32(c, %du + (uint32_t)nd_slide); c->eip = %du + (uint32_t)nd_slide; return %d;'
                    % (ret, tgt, gidx(tgt))]
        return ['    push32(c, %du + (uint32_t)nd_slide); c->eip = %s; return -1;' % (ret, dyn)]
    # ret
    _, pb = exitd
    if pb: return ['    { uint32_t r=pop32(c); c->regs[4]+=%du; c->eip=r; return -1; }' % pb]
    return ['    c->eip = pop32(c); return -1;']

o = open(OUT, 'w')
o.write('/* auto-generated by x86toc.py - do not edit */\n')
o.write('#ifdef WEBWINE_GENBLOCKS\n')
for lead in sb:
    lines, exitd, end, ninsn = blocks[lead]
    o.write('static int gblk_%08x(struct x86cpu *c){\n' % lead)
    for ln in lines: o.write(ln + '\n')
    for ln in emit_exit(exitd): o.write(ln + '\n')
    o.write('}\n')
o.write('struct genblk { uint32_t va, end; uint16_t ninsn; int (*fn)(struct x86cpu*); };\n')
o.write('static const struct genblk g_genblk[] = {\n')
for lead in sb:
    lines, exitd, end, ninsn = blocks[lead]
    o.write('  { 0x%08xu, 0x%08xu, %d, gblk_%08x },\n' % (lead, end, min(ninsn,65535), lead))
o.write('  { 0, 0, 0, 0 }\n};\n')
o.write('#endif\n')
o.close()
print('translated %d/%d basic blocks' % (len(blocks), len(leaders)))
print('top unhandled opcodes (block-killers):', file=sys.stderr)
for mn, n in skipmn.most_common(30): print('  %-8s %d' % (mn, n), file=sys.stderr)
