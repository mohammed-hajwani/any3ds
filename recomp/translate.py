"""Translate ARM instructions into C statements that operate on `Ctx *ctx`.

Supported:
- M1: data processing (mov mvn add adc sub sbc rsb and orr eor bic cmp cmn
  tst teq, with and without S, immediate shifts), the lsl/lsr/asr/ror/rrx
  shift mnemonics, uxtb, b/bl/bx lr with any condition.
- M2: ldr/str/ldrb/strb/ldrh/strh/ldrsb/ldrsh in every addressing mode,
  ldm/stm (ia/ib/da/db, push/pop), literal pools, gcc's switch jump table
  (`add pc, pc, rX, lsl #2`), nop.

Anything else is a hard error -- never skip an instruction.
"""

import struct
from dataclasses import dataclass, field

from capstone import CS_MODE_THUMB
from capstone.arm import (
    ARM_CC_AL, ARM_CC_EQ, ARM_CC_GE, ARM_CC_GT, ARM_CC_HI, ARM_CC_HS,
    ARM_CC_LE, ARM_CC_LO, ARM_CC_LS, ARM_CC_LT, ARM_CC_MI, ARM_CC_NE,
    ARM_CC_PL, ARM_CC_VC, ARM_CC_VS,
    ARM_INS_ADC, ARM_INS_ADD, ARM_INS_ADR, ARM_INS_AND, ARM_INS_ASR, ARM_INS_B,
    ARM_INS_BIC, ARM_INS_BL, ARM_INS_BLX, ARM_INS_BX, ARM_INS_CLREX, ARM_INS_CLZ, ARM_INS_CMN, ARM_INS_CMP,
    ARM_INS_EOR, ARM_INS_FMSTAT, ARM_INS_HINT, ARM_INS_LDM, ARM_INS_LDMDA, ARM_INS_LDMDB, ARM_INS_LDMIB,
    ARM_INS_LDR, ARM_INS_LDRB, ARM_INS_LDRD, ARM_INS_LDREX, ARM_INS_LDREXB, ARM_INS_LDREXH,
    ARM_INS_LDRH, ARM_INS_LDRSB, ARM_INS_LDRSH, ARM_INS_MCR, ARM_INS_MRC,
    ARM_INS_LSL, ARM_INS_LSR, ARM_INS_MLA, ARM_INS_MOV, ARM_INS_MOVS, ARM_INS_MUL, ARM_INS_MVN, ARM_INS_NOP,
    ARM_INS_ORR, ARM_INS_POP, ARM_INS_PUSH, ARM_INS_ROR, ARM_INS_RRX,
    ARM_INS_REV, ARM_INS_REV16, ARM_INS_REVSH, ARM_INS_RSB, ARM_INS_RSC, ARM_INS_SBC, ARM_INS_SEL,
    ARM_INS_SMLAL, ARM_INS_STREX, ARM_INS_STREXB, ARM_INS_STREXH, ARM_INS_SVC, ARM_INS_UADD8, ARM_INS_UDF,
    ARM_INS_SMULL, ARM_INS_STM, ARM_INS_STRD, ARM_INS_SXTB, ARM_INS_SXTH, ARM_INS_UMLAL, ARM_INS_UMULL,
    ARM_INS_UXTH, ARM_INS_STMDA, ARM_INS_STMDB,
    ARM_INS_STMIB, ARM_INS_STR, ARM_INS_STRB, ARM_INS_STRH, ARM_INS_SUB,
    ARM_INS_TEQ, ARM_INS_TST, ARM_INS_UXTB,
    ARM_INS_VABS, ARM_INS_VADD, ARM_INS_VCMP, ARM_INS_VCMPE, ARM_INS_VCVT,
    ARM_INS_VCVTR, ARM_INS_VDIV, ARM_INS_VLDMDB, ARM_INS_VLDMIA, ARM_INS_VLDR,
    ARM_INS_VMLA, ARM_INS_VMLS, ARM_INS_VMOV, ARM_INS_VMRS, ARM_INS_VMUL,
    ARM_INS_VNEG, ARM_INS_VNMLA, ARM_INS_VNMLS, ARM_INS_VNMUL, ARM_INS_VPOP,
    ARM_INS_VPUSH, ARM_INS_VSQRT, ARM_INS_VSTMDB, ARM_INS_VSTMIA, ARM_INS_VSTR,
    ARM_INS_VSUB,
    ARM_OP_IMM, ARM_OP_MEM, ARM_OP_REG,
    ARM_SFT_ASR, ARM_SFT_ASR_REG, ARM_SFT_INVALID, ARM_SFT_LSL, ARM_SFT_LSL_REG,
    ARM_SFT_LSR, ARM_SFT_LSR_REG, ARM_SFT_ROR, ARM_SFT_ROR_REG, ARM_SFT_RRX,
)

from .emit_c import c_name


class UnsupportedInstruction(Exception):
    def __init__(self, insn, reason):
        self.addr = insn.address
        self.mnemonic = insn.mnemonic
        text = f'{insn.mnemonic} {insn.op_str}'.strip()
        super().__init__(f'{insn.address:08x}: unsupported instruction `{text}`: {reason}')


# Capstone prints r9-r12 by their APCS aliases.
_REGS = {f'r{i}': i for i in range(16)}
_REGS.update(sb=9, sl=10, fp=11, ip=12, sp=13, lr=14, pc=15)

SP, LR, PC = 13, 14, 15

_CONDS = {
    ARM_CC_EQ: 'eq', ARM_CC_NE: 'ne', ARM_CC_HS: 'cs', ARM_CC_LO: 'cc',
    ARM_CC_MI: 'mi', ARM_CC_PL: 'pl', ARM_CC_VS: 'vs', ARM_CC_VC: 'vc',
    ARM_CC_HI: 'hi', ARM_CC_LS: 'ls', ARM_CC_GE: 'ge', ARM_CC_LT: 'lt',
    ARM_CC_GT: 'gt', ARM_CC_LE: 'le',
}


@dataclass
class FunctionContext:
    """What the translator knows while working through one function."""
    fn: object                          # elf_loader.Function
    entries: dict                       # guest address -> Function, for bl
    program: object = None              # elf_loader.Program, for literal pools
    code_addrs: set = field(default_factory=set)   # instruction starts in this function
    labels: set = field(default_factory=set)
    uses_ret: bool = False              # emit `const uint32_t ret = lr;`
    terminates: bool = False            # last insn was an unconditional return/goto

    @property
    def start(self):
        return self.fn.addr

    @property
    def end(self):
        return self.fn.addr + len(self.fn.code)


@dataclass
class TranslatedFunction:
    fn: object
    body: list                          # [(addr, comment, [C lines])], code and data
    labels: set
    uses_ret: bool


# ---------------------------------------------------------------- operands

def _reg_num(insn, reg):
    return _REGS[insn.reg_name(reg)]


def _imm(value):
    value &= 0xFFFFFFFF
    return f'{value}u' if value < 10 else f'0x{value:x}u'


def _is_thumb(insn):
    return bool(insn._cs.mode & CS_MODE_THUMB)


def _pc(insn):
    """What reading r15 gives: this instruction + 8 in ARM mode, + 4 in
    Thumb (a leftover of the original 3-stage pipeline)."""
    return insn.address + (4 if _is_thumb(insn) else 8)


def _read_reg(insn, reg):
    n = _reg_num(insn, reg)
    if n == PC:
        return _imm(_pc(insn))
    return f'ctx->r[{n}]'


def _encoding(insn):
    return int.from_bytes(insn.bytes, 'little')


def _sets_flags(insn):
    """Whether a data-processing instruction writes the flags, from its encoding.

    Don't trust Capstone's update_flags: it is True for adc/sbc/rsc even
    without S (they *read* the carry, they don't write the flags).
    """
    if not _is_thumb(insn):
        return bool((_encoding(insn) >> 20) & 1)        # the S bit
    # Thumb-1 has no S bit; it depends on the 16-bit format:
    h = _encoding(insn)
    if h >> 14 == 0b00:         # shift by immediate, add/sub, mov/cmp/add/sub #imm8
        return True
    if h >> 10 == 0b010000:     # register ALU ops (ands, eors, lsls rd, rs, ..., muls)
        return True
    if h >> 10 == 0b010001:     # high-register add/cmp/mov: only cmp sets flags
        return (h >> 8) & 3 == 0b01
    return False                # add rd, pc/sp, #imm; add/sub sp, #imm


def _dest_reg(insn, op):
    if op.type != ARM_OP_REG:
        raise UnsupportedInstruction(insn, 'unexpected operand shape')
    n = _reg_num(insn, op.reg)
    if n == PC:
        raise UnsupportedInstruction(insn, 'writes pc (computed branches come later)')
    return n


def _shift_imm(insn, x, kind, n):
    """Shift `x` by the constant `n`: returns (C expression, carry-out expression).

    The carry-out is the last bit shifted out; None means the shifter
    leaves C unchanged. C leaves shifts by >= 32 undefined, so the ARM
    encodings that mean "shift by 32" (lsr #32, asr #32) are special-cased.
    """
    if kind == ARM_SFT_INVALID or (kind == ARM_SFT_LSL and n == 0):
        return x, None
    if kind == ARM_SFT_LSL and 1 <= n <= 31:
        return f'({x} << {n})', f'(({x} >> {32 - n}) & 1u)'
    if kind == ARM_SFT_LSR and 1 <= n <= 32:
        if n == 32:
            return '0u', f'({x} >> 31)'
        return f'({x} >> {n})', f'(({x} >> {n - 1}) & 1u)'
    if kind == ARM_SFT_ASR and 1 <= n <= 32:
        # Shifting right by 31 already fills every bit with the sign, so
        # asr #32 gives the same value as asr #31 (carry is bit 31).
        if n == 32:
            return f'(uint32_t)((int32_t){x} >> 31)', f'({x} >> 31)'
        return f'(uint32_t)((int32_t){x} >> {n})', f'(({x} >> {n - 1}) & 1u)'
    if kind == ARM_SFT_ROR and 1 <= n <= 31:
        return f'(({x} >> {n}) | ({x} << {32 - n}))', f'(({x} >> {n - 1}) & 1u)'
    if kind == ARM_SFT_RRX:
        # Rotate right by one *through* the carry flag: C goes into bit 31,
        # bit 0 comes out as the new C.
        return f'(({x} >> 1) | (ctx->c << 31))', f'({x} & 1u)'
    if kind >= ARM_SFT_ASR_REG:
        raise UnsupportedInstruction(insn, 'shift amount in a register')
    raise UnsupportedInstruction(insn, f'shift type {kind} by {n}')


_REG_SHIFTS = {ARM_SFT_LSL_REG: 'lsl', ARM_SFT_LSR_REG: 'lsr', ARM_SFT_ASR_REG: 'asr', ARM_SFT_ROR_REG: 'ror'}
_IMM_TO_REG_SHIFT = {ARM_SFT_LSL: ARM_SFT_LSL_REG, ARM_SFT_LSR: ARM_SFT_LSR_REG,
                     ARM_SFT_ASR: ARM_SFT_ASR_REG, ARM_SFT_ROR: ARM_SFT_ROR_REG}


def _shift_reg(insn, x_reg, kind, s_reg):
    """Shift by an amount held in a register.

    The amount isn't known until runtime, and ARM's rules for it (only the
    bottom byte counts; 0, 32 and >32 each behave differently) live in the
    runtime's shift_* helpers. Returns (expression, carry marker): the
    marker says "the helper can set C itself" (see _pick_carry).
    """
    if PC in (_reg_num(insn, x_reg), _reg_num(insn, s_reg)):
        # With a register-specified shift, pc reads as +12 in ARM mode.
        raise UnsupportedInstruction(insn, 'pc with a register-specified shift')
    x, s = _read_reg(insn, x_reg), _read_reg(insn, s_reg)
    name = _REG_SHIFTS[kind]
    return f'shift_{name}(ctx, {x}, {s}, 0)', ('helper', f'shift_{name}(ctx, {x}, {s}, 1)')


def _pick_carry(m, carry, logical_s):
    """Resolve a register-shift carry marker. Only logical ops with S take
    the shifter's carry; then the helper variant that writes C is used."""
    if isinstance(carry, tuple):
        return (carry[1], None) if logical_s else (m, None)
    return m, carry


def _operand2(insn, op):
    """ARM's flexible second operand: returns (C expression, carry-out or None)."""
    if op.type == ARM_OP_IMM:
        # Capstone has already expanded the encoding (8 bits rotated right
        # by 2 * rot). If it was rotated at all, the shifter's carry-out
        # is bit 31 of the result; otherwise C is left alone.
        word = int.from_bytes(insn.bytes, 'little')
        rotated = (word >> 25) & 1 and (word >> 8) & 0xF
        value = op.imm & 0xFFFFFFFF
        carry = _imm(value >> 31) if rotated else None
        return _imm(value), carry
    if op.type == ARM_OP_REG:
        if op.shift.type in _REG_SHIFTS:
            # Capstone puts the amount register's id in shift.value.
            return _shift_reg(insn, op.reg, op.shift.type, op.shift.value)
        return _shift_imm(insn, _read_reg(insn, op.reg), op.shift.type, op.shift.value)
    raise UnsupportedInstruction(insn, f'operand type {op.type}')


# ---------------------------------------------------------------- data processing

# Arithmetic ops with S get NZCV from the adder; logical ops get N and Z
# from the result and C from the barrel shifter.
_ARITH_FLAGS = {
    'add': 'add_flags(ctx, {n}, {m})',
    'adc': 'adc_flags(ctx, {n}, {m})',
    'sub': 'sub_flags(ctx, {n}, {m})',
    'sbc': 'sbc_flags(ctx, {n}, {m})',
    'rsb': 'sub_flags(ctx, {m}, {n})',
    'rsc': 'sbc_flags(ctx, {m}, {n})',
}

# id: (C expression, writes Rd, flag kind)
_BINARY = {
    ARM_INS_ADD: ('{n} + {m}', True, 'add'),
    ARM_INS_ADC: ('{n} + {m} + ctx->c', True, 'adc'),
    ARM_INS_SUB: ('{n} - {m}', True, 'sub'),
    ARM_INS_SBC: ('{n} - {m} - (1u - ctx->c)', True, 'sbc'),
    ARM_INS_RSB: ('{m} - {n}', True, 'rsb'),
    ARM_INS_RSC: ('{m} - {n} - (1u - ctx->c)', True, 'rsc'),
    ARM_INS_AND: ('{n} & {m}', True, 'logic'),
    ARM_INS_ORR: ('{n} | {m}', True, 'logic'),
    ARM_INS_EOR: ('{n} ^ {m}', True, 'logic'),
    ARM_INS_BIC: ('{n} & ~{m}', True, 'logic'),
    ARM_INS_CMP: (None, False, 'sub'),
    ARM_INS_CMN: (None, False, 'add'),
    ARM_INS_TST: ('{n} & {m}', False, 'logic'),
    ARM_INS_TEQ: ('{n} ^ {m}', False, 'logic'),
}

_UNARY = {
    ARM_INS_MOV: '{m}',
    ARM_INS_MOVS: '{m}',        # Thumb `movs rd, rm` (really lsls rd, rm, #0)
    ARM_INS_MVN: '~{m}',
}

_SHIFT_MNEMONICS = {
    ARM_INS_LSL: ARM_SFT_LSL,
    ARM_INS_LSR: ARM_SFT_LSR,
    ARM_INS_ASR: ARM_SFT_ASR,
    ARM_INS_ROR: ARM_SFT_ROR,
    ARM_INS_RRX: ARM_SFT_RRX,
}


def _emit_dp(rd, expr, kind, setflags, carry, n=None, m=None):
    if not setflags:
        return [f'ctx->r[{rd}] = {expr};']

    if kind != 'logic':
        call = _ARITH_FLAGS[kind].format(n=n, m=m)
        return [f'ctx->r[{rd}] = {call};'] if rd is not None else [f'{call};']

    if carry is None:
        if rd is None:
            return [f'set_nz(ctx, {expr});']
        return [f'ctx->r[{rd}] = {expr};', f'set_nz(ctx, ctx->r[{rd}]);']

    # Compute the result before touching C (rrx reads the old C), and the
    # carry before writing Rd (Rd may be the register being shifted).
    lines = ['{', f'    uint32_t res = {expr};', f'    ctx->c = {carry};']
    if rd is not None:
        lines.append(f'    ctx->r[{rd}] = res;')
    lines += ['    set_nz(ctx, res);', '}']
    return lines


def _binary(insn, fc):
    template, writes_rd, kind = _BINARY[insn.id]
    ops = list(insn.operands)
    if insn.id == ARM_INS_ADD and len(ops) == 3 and ops[0].type == ARM_OP_REG \
            and _reg_num(insn, ops[0].reg) == PC:
        return _jump_table(insn, fc)
    if writes_rd:
        if len(ops) == 2:               # `add rd, op2` is short for `add rd, rd, op2`
            ops = [ops[0]] + ops
        if len(ops) != 3:
            raise UnsupportedInstruction(insn, 'unexpected operand shape')
        rd = _dest_reg(insn, ops[0])
        rn_op, op2 = ops[1], ops[2]
    else:
        if len(ops) != 2:
            raise UnsupportedInstruction(insn, 'unexpected operand shape')
        rd = None
        rn_op, op2 = ops
    if rn_op.type != ARM_OP_REG:
        raise UnsupportedInstruction(insn, 'unexpected operand shape')

    if _is_thumb(insn) and _reg_num(insn, rn_op.reg) == PC and _encoding(insn) >> 11 == 0b10100:
        # Thumb `add rd, pc, #imm` (adr) uses pc aligned down to 4.
        n = _imm(_pc(insn) & ~3)
    else:
        n = _read_reg(insn, rn_op.reg)
    setflags = _sets_flags(insn) or not writes_rd
    m, carry = _pick_carry(*_operand2(insn, op2), kind == 'logic' and setflags)
    expr = template.format(n=n, m=m) if template else None
    return _emit_dp(rd, expr, kind, setflags, carry, n, m)


def _unary(insn, fc):
    ops = insn.operands
    if len(ops) != 2:
        raise UnsupportedInstruction(insn, 'unexpected operand shape')
    if _is_thumb(insn) and insn.id == ARM_INS_MOV and ops[0].type == ARM_OP_REG \
            and _reg_num(insn, ops[0].reg) == PC:
        return _computed_jump(insn, fc, ops[1])
    rd = _dest_reg(insn, ops[0])
    setflags = _sets_flags(insn)
    m, carry = _pick_carry(*_operand2(insn, ops[1]), setflags)
    return _emit_dp(rd, _UNARY[insn.id].format(m=m), 'logic', setflags, carry)


def _shift_mnemonic(insn, fc):
    """`lsrs r3, r3, #1` etc.: Capstone's names for mov with a shifted operand.

    The operand layouts Capstone uses:
      ARM    lsl rd, rm, #n  -> [rd, rm (shift lsl n)]
      ARM    lsl rd, rm, rs  -> [rd, rm, rs]
      ARM    rrx rd, rm      -> [rd, rm]
      Thumb  lsls rd, rm, #n -> [rd, rm, #n]
      Thumb  lsls rd, rs     -> [rd, rs]          (rd = rd << rs)
    """
    ops = insn.operands
    kind = _SHIFT_MNEMONICS[insn.id]
    if any(op.type not in (ARM_OP_REG, ARM_OP_IMM) for op in ops) or ops[0].type != ARM_OP_REG:
        raise UnsupportedInstruction(insn, 'unexpected operand shape')
    rd = _dest_reg(insn, ops[0])
    thumb = _is_thumb(insn)

    if insn.id == ARM_INS_RRX and len(ops) == 2:
        m, carry = _shift_imm(insn, _read_reg(insn, ops[1].reg), ARM_SFT_RRX, 1)
    elif len(ops) == 3 and ops[2].type == ARM_OP_IMM:
        m, carry = _shift_imm(insn, _read_reg(insn, ops[1].reg), kind, ops[2].imm)
    elif len(ops) == 3 and ops[2].type == ARM_OP_REG:
        m, carry = _shift_reg(insn, ops[1].reg, _IMM_TO_REG_SHIFT[kind], ops[2].reg)
    elif len(ops) == 2 and not thumb and ops[1].shift.type == kind:
        m, carry = _shift_imm(insn, _read_reg(insn, ops[1].reg), kind, ops[1].shift.value)
    elif len(ops) == 2 and thumb and ops[1].type == ARM_OP_REG:
        m, carry = _shift_reg(insn, ops[0].reg, _IMM_TO_REG_SHIFT[kind], ops[1].reg)
    else:
        raise UnsupportedInstruction(insn, 'unexpected operand shape')
    setflags = _sets_flags(insn)
    m, carry = _pick_carry(m, carry, setflags)
    return _emit_dp(rd, m, 'logic', setflags, carry)


def _adr(insn, fc):
    """adr rd, #off: an address relative to pc, known at recompile time.
    Thumb uses pc (this instruction + 4) aligned down to 4; ARM uses + 8."""
    ops = insn.operands
    if len(ops) != 2 or ops[1].type != ARM_OP_IMM:
        raise UnsupportedInstruction(insn, 'unexpected operand shape')
    rd = _dest_reg(insn, ops[0])
    base = _pc(insn) & ~3 if _is_thumb(insn) else _pc(insn)
    return [f'ctx->r[{rd}] = {_imm(base + ops[1].imm)};']


def _mul(insn, fc):
    """mul/muls/mla: the low 32 bits of the product (plus rn for mla). With S, N and Z come from
    the result; C and V are unchanged on ARMv6."""
    ops = insn.operands
    nops = 4 if insn.id == ARM_INS_MLA else (2, 3)
    if any(op.type != ARM_OP_REG for op in ops) or len(ops) not in (nops if isinstance(nops, tuple) else (nops,)):
        raise UnsupportedInstruction(insn, 'unexpected operand shape')
    rd = _dest_reg(insn, ops[0])
    sources = [ops[0], ops[1]] if len(ops) == 2 else list(ops[1:])
    if any(_reg_num(insn, op.reg) == PC for op in sources):
        raise UnsupportedInstruction(insn, 'pc as a multiply operand is unpredictable')
    a, b = (_read_reg(insn, op.reg) for op in sources[:2])
    expr = f'{a} * {b}'
    if insn.id == ARM_INS_MLA:                  # multiply-accumulate: rd = rm * rs + rn
        expr += f' + {_read_reg(insn, sources[2].reg)}'
    return _emit_dp(rd, expr, 'logic', _sets_flags(insn), None)


_EXTENDS = {
    ARM_INS_UXTB: '{x} & 0xffu',
    ARM_INS_UXTH: '{x} & 0xffffu',
    ARM_INS_SXTB: '(uint32_t)(int32_t)(int8_t){x}',
    ARM_INS_SXTH: '(uint32_t)(int32_t)(int16_t){x}',
}


def _extend(insn, fc):
    """uxtb/uxth/sxtb/sxth: zero/sign-extend the low byte or halfword, after
    an optional rotation by 8/16/24 (ARM only)."""
    ops = insn.operands
    if len(ops) != 2 or ops[1].type != ARM_OP_REG:
        raise UnsupportedInstruction(insn, 'unexpected operand shape')
    rd = _dest_reg(insn, ops[0])
    x, _ = _shift_imm(insn, _read_reg(insn, ops[1].reg), ops[1].shift.type, ops[1].shift.value)
    if x != _read_reg(insn, ops[1].reg):
        x = f'({x})'
    return [f'ctx->r[{rd}] = {_EXTENDS[insn.id].format(x=x)};']


_REVERSES = {
    ARM_INS_REV: '__builtin_bswap32({x})',
    ARM_INS_REV16: '((({x} & 0x00ff00ffu) << 8) | (({x} >> 8) & 0x00ff00ffu))',
    ARM_INS_REVSH: '(uint32_t)(int32_t)(int16_t)((({x} & 0xffu) << 8) | (({x} >> 8) & 0xffu))',
}


def _reverse(insn, fc):
    """rev (byte swap), rev16 (swap within each halfword), revsh (swap the
    low halfword and sign-extend)."""
    ops = insn.operands
    if len(ops) != 2 or any(op.type != ARM_OP_REG for op in ops):
        raise UnsupportedInstruction(insn, 'unexpected operand shape')
    rd = _dest_reg(insn, ops[0])
    return [f'ctx->r[{rd}] = {_REVERSES[insn.id].format(x=_read_reg(insn, ops[1].reg))};']


def _clz(insn, fc):
    """Count leading zeros (ARM only); clz of 0 is 32."""
    ops = insn.operands
    if len(ops) != 2 or any(op.type != ARM_OP_REG for op in ops):
        raise UnsupportedInstruction(insn, 'unexpected operand shape')
    rd = _dest_reg(insn, ops[0])
    return [f'ctx->r[{rd}] = arm_clz({_read_reg(insn, ops[1].reg)});']


_LONG_MUL = {   # id: (signed, accumulate)
    ARM_INS_UMULL: (False, False), ARM_INS_SMULL: (True, False),
    ARM_INS_UMLAL: (False, True), ARM_INS_SMLAL: (True, True),
}


def _long_mul(insn, fc):
    """umull/smull/umlal/smlal: 32 x 32 -> 64-bit product into rdlo:rdhi
    (plus the 64-bit value already there, for the accumulating forms).
    With S, N and Z come from the 64-bit result."""
    signed, acc = _LONG_MUL[insn.id]
    ops = insn.operands
    if len(ops) != 4 or any(op.type != ARM_OP_REG for op in ops):
        raise UnsupportedInstruction(insn, 'unexpected operand shape')
    lo, hi = _dest_reg(insn, ops[0]), _dest_reg(insn, ops[1])
    if lo == hi:
        raise UnsupportedInstruction(insn, 'rdlo == rdhi is unpredictable')
    n, m = (_read_reg(insn, op.reg) for op in ops[2:])
    prod = f'(uint64_t)((int64_t)(int32_t){n} * (int32_t){m})' if signed else f'(uint64_t){n} * {m}'
    if acc:
        prod = f'((uint64_t)ctx->r[{hi}] << 32 | ctx->r[{lo}]) + {prod}'
    lines = ['{', f'    uint64_t p = {prod};',
             f'    ctx->r[{lo}] = (uint32_t)p;', f'    ctx->r[{hi}] = (uint32_t)(p >> 32);']
    if _sets_flags(insn):
        lines += ['    ctx->n = (uint32_t)(p >> 63);', '    ctx->z = p == 0;']
    lines.append('}')
    return lines


# ---------------------------------------------------------------- control flow

def _branch_target(insn):
    (op,) = insn.operands
    if op.type != ARM_OP_IMM:
        raise UnsupportedInstruction(insn, 'branch to a register')
    return op.imm & 0xFFFFFFFF


def direct_branch_target(insn):
    """Target of a b/bl/blx #imm, or None. Used for function discovery."""
    if insn.id in (ARM_INS_B, ARM_INS_BL, ARM_INS_BLX) and len(insn.operands) == 1 \
            and insn.operands[0].type == ARM_OP_IMM:
        return insn.operands[0].imm & 0xFFFFFFFF
    return None


def _b(insn, fc):
    target = _branch_target(insn)
    fc.terminates = True
    if fc.start <= target < fc.end:
        fc.labels.add(target)
        return [f'goto L_{target:08x};']
    # A tail call: jump to another function, which then returns straight to
    # our caller (lr is untouched). In C: call it, then return.
    callee = fc.entries.get(target)
    if callee is None:
        raise UnsupportedInstruction(insn, f'branch to {target:08x}, which is not a known function')
    if callee.thumb != _is_thumb(insn):
        raise UnsupportedInstruction(insn, f'branch to {callee.name} would need a mode switch')
    return [f'{c_name(target)}(ctx);  /* tail call: {callee.name} */', 'return;']


def _bl(insn, fc):
    """bl, and blx #imm (a call that also switches ARM <-> Thumb).

    The callee's mode is known statically, so either way it's a plain C
    call. The return address goes in lr exactly as on hardware; in Thumb
    it has bit 0 set, so the callee's `bx lr` knows to return to Thumb.
    """
    op = insn.operands[0]
    ret = (insn.address + insn.size) | int(_is_thumb(insn))
    if insn.id == ARM_INS_BLX and op.type == ARM_OP_REG:
        # An indirect call (function pointer, vtable): the target is only
        # known at runtime, so look it up in the table of recompiled
        # functions. Bit 0 of the address selects Thumb, as on hardware.
        if _reg_num(insn, op.reg) == PC:
            raise UnsupportedInstruction(insn, 'blx pc is unpredictable')
        return ['{', f'    uint32_t target = {_read_reg(insn, op.reg)};',
                f'    ctx->r[14] = {_imm(ret)};', '    recomp_call(ctx, target);', '}']
    target = _branch_target(insn)
    callee = fc.entries.get(target)
    if callee is None:
        raise UnsupportedInstruction(
            insn, f'{insn.mnemonic} target {target:08x} is not the start of a known function')
    expected_mode = (not _is_thumb(insn)) if insn.id == ARM_INS_BLX else _is_thumb(insn)
    if callee.thumb != expected_mode:
        mode = 'Thumb' if callee.thumb else 'ARM'
        raise UnsupportedInstruction(insn, f'calls {callee.name}, a {mode} function, without switching mode')
    return [f'ctx->r[14] = {_imm(ret)};', f'{c_name(target)}(ctx);  /* {callee.name} */']


def _bx(insn, fc):
    (op,) = insn.operands
    if op.type == ARM_OP_REG and _reg_num(insn, op.reg) == LR:
        # Returns to ARM or Thumb according to bit 0 of lr ("interworking");
        # RETURN_TO checks it matches what our caller passed in.
        fc.uses_ret = True
        fc.terminates = True
        return ['RETURN_TO(ctx->r[14]);']
    if op.type == ARM_OP_REG and _reg_num(insn, op.reg) != PC:
        # An indirect tail call (e.g. through a function pointer): look the
        # target up, call it, and return to our caller.
        fc.terminates = True
        return [f'recomp_call(ctx, {_read_reg(insn, op.reg)});  /* indirect tail call */', 'return;']
    raise UnsupportedInstruction(insn, 'bx pc')


def _computed_jump(insn, fc, op):
    """Thumb `mov pc, rX`: gcc's Thumb switch, jumping to an address loaded
    from a table. Becomes a C switch over every instruction address in the
    function; anything else is a jump we never recompiled."""
    if op.type != ARM_OP_REG or _reg_num(insn, op.reg) == PC:
        raise UnsupportedInstruction(insn, 'writes pc (computed branches come later)')
    value = _read_reg(insn, op.reg)
    # Thumb `mov pc` doesn't switch mode on ARMv6; bit 0 is ignored.
    lines = [f'switch ({value} & ~1u) {{']
    for addr in sorted(fc.code_addrs):
        fc.labels.add(addr)
        lines.append(f'case {_imm(addr)}: goto L_{addr:08x};')
    lines += [f'default: recomp_bad_jump(ctx, {_imm(insn.address)}, {value});', '}']
    fc.terminates = True
    return lines


def _jump_table(insn, fc):
    """gcc's switch dispatch: `add pc, pc, rX, lsl #2`.

    pc reads as this instruction + 8, so this jumps to addr + 8 + 4*rX,
    into a row of branches/returns that follows. Becomes a C switch over
    every code address from there to the end of the function.
    """
    _, rn_op, op2 = insn.operands
    if (_is_thumb(insn) or _sets_flags(insn) or rn_op.type != ARM_OP_REG or _reg_num(insn, rn_op.reg) != PC
            or op2.type != ARM_OP_REG or op2.shift.type != ARM_SFT_LSL or op2.shift.value != 2):
        raise UnsupportedInstruction(insn, 'writes pc (computed branches come later)')
    index = _read_reg(insn, op2.reg)
    base = insn.address + 8
    lines = [f'switch ({index}) {{']
    for k in range((fc.end - base) // 4):
        target = base + 4 * k
        if target in fc.code_addrs:         # data words in range fall to default
            fc.labels.add(target)
            lines.append(f'case {k}: goto L_{target:08x};')
    lines += [f'default: recomp_bad_jump(ctx, {_imm(insn.address)}, {index});', '}']
    fc.terminates = True
    return lines


# ---------------------------------------------------------------- loads / stores

# id: (read helper, sign-extend from, size)
_LOADS = {
    ARM_INS_LDR: ('read32', None, 4),
    ARM_INS_LDRB: ('read8', None, 1),
    ARM_INS_LDRH: ('read16', None, 2),
    ARM_INS_LDRSB: ('read8', 'int8_t', 1),
    ARM_INS_LDRSH: ('read16', 'int16_t', 2),
}
_STORES = {
    ARM_INS_STR: 'write32',
    ARM_INS_STRB: 'write8',
    ARM_INS_STRH: 'write16',
}


@dataclass
class _Addr:
    """A single load/store's address, in one of ARM's three modes:

      offset      [rn, x]    address = rn + x
      pre-index   [rn, x]!   address = rn + x, then rn = address
      post-index  [rn], x    address = rn,     then rn = rn + x
    """
    rn: int
    base: str                       # C expression for rn
    offset: tuple = None            # ('+'|'-', C expression), or None for zero
    mode: str = 'offset'
    const: int = None               # the address, when it's known at recompile time

    def apply(self, base):
        return base if self.offset is None else f'{base} {self.offset[0]} {self.offset[1]}'


def _offset(insn, op, subtracted):
    """An offset operand: (sign, C expression), or None for zero."""
    if op.type == ARM_OP_IMM:
        value = -op.imm if subtracted else op.imm
        return None if value == 0 else ('-' if value < 0 else '+', _imm(abs(value)))
    if op.type == ARM_OP_REG:
        x, _ = _shift_imm(insn, _read_reg(insn, op.reg), op.shift.type, op.shift.value)
        return ('-' if subtracted else '+', x)
    raise UnsupportedInstruction(insn, f'offset operand type {op.type}')


def _address(insn, mem_index=1):
    """Decode the memory operand (operand `mem_index`; 2 for ldrd/strd)."""
    ops = insn.operands
    if len(ops) <= mem_index or ops[mem_index].type != ARM_OP_MEM:
        raise UnsupportedInstruction(insn, 'unexpected operand shape')
    mem = ops[mem_index]
    rn = _reg_num(insn, mem.mem.base)
    addr = _Addr(rn, _read_reg(insn, mem.mem.base))

    # Bit 24 (P) of the encoding says pre/offset (1) or post-index (0), for
    # both the word/byte and halfword/signed-byte encodings. Don't trust
    # Capstone's post_index flag: it's always False for ldrh/strh/ldrsb/ldrsh.
    # Thumb-1 loads/stores have no pre/post-index forms: always plain offset.
    post = False if _is_thumb(insn) else not (_encoding(insn) >> 24) & 1
    if post != (len(ops) == mem_index + 2):
        raise UnsupportedInstruction(insn, 'unexpected operand shape')

    if post:
        # Capstone gives the post-index offset as an extra operand.
        off = ops[mem_index + 1]
        addr.offset, addr.mode = _offset(insn, off, off.subtracted), 'post'
    elif mem.mem.index:
        x, _ = _shift_imm(insn, _read_reg(insn, mem.mem.index), mem.shift.type, mem.shift.value)
        addr.offset = ('-' if mem.subtracted else '+', x)
        addr.mode = 'pre' if insn.writeback and not _is_thumb(insn) else 'offset'
    else:
        disp = mem.mem.disp
        addr.offset = None if disp == 0 else ('-' if disp < 0 else '+', _imm(abs(disp)))
        addr.mode = 'pre' if insn.writeback and not _is_thumb(insn) else 'offset'
        if rn == PC:
            # pc-relative: the address is a constant we can compute now.
            # In Thumb, literal loads use pc (this instruction + 4) aligned
            # down to a multiple of 4.
            base = _pc(insn) & ~3 if _is_thumb(insn) else _pc(insn)
            addr.const = (base + disp) & 0xFFFFFFFF

    if rn == PC and addr.mode != 'offset':
        raise UnsupportedInstruction(insn, 'pc as base with writeback is unpredictable')
    return addr


def _with_writeback(addr, rt, access):
    """Emit a pre/post-indexed access. Both the address and the new base are
    computed before the access, from the original registers."""
    first = addr.apply(addr.base) if addr.mode == 'pre' else addr.base
    new_base = 'a' if addr.mode == 'pre' else addr.apply('a')
    return ['{',
            f'    uint32_t a = {first};',
            f'    uint32_t wb = {new_base};',
            f'    {access};',
            f'    ctx->r[{addr.rn}] = wb;',
            '}']


def _load(insn, fc):
    helper, sign, size = _LOADS[insn.id]
    rt = _reg_num(insn, insn.operands[0].reg)
    addr = _address(insn)

    if rt == PC:
        # `ldr pc, [sp], #4` is a single-register pop {pc}: a return.
        if (insn.id == ARM_INS_LDR and addr.rn == SP and addr.mode == 'post'
                and addr.offset == ('+', '4u')):
            fc.uses_ret = True
            fc.terminates = True
            return ['{', '    uint32_t pc = read32(ctx, ctx->r[13]);', '    ctx->r[13] += 4u;',
                    '    RETURN_TO(pc);', '}']
        raise UnsupportedInstruction(insn, 'load into pc (indirect jumps come later)')
    if addr.mode != 'offset' and rt == addr.rn:
        raise UnsupportedInstruction(insn, 'writeback to the loaded register is unpredictable')

    if addr.const is not None:
        data = fc.program.literal(addr.const, size) if fc.program else None
        if data is not None:
            # A literal pool: read-only data next to the code. Fold it.
            value = int.from_bytes(data, 'little')
            if sign == 'int8_t' and value & 0x80:
                value |= 0xFFFFFF00
            if sign == 'int16_t' and value & 0x8000:
                value |= 0xFFFF0000
            return [f'ctx->r[{rt}] = {_imm(value)};  /* literal @{addr.const:08x} */']

    def value(a):
        v = f'{helper}(ctx, {a})'
        return f'(uint32_t)(int32_t)({sign}){v}' if sign else v

    if addr.mode == 'offset':
        where = _imm(addr.const) if addr.const is not None else addr.apply(addr.base)
        return [f'ctx->r[{rt}] = {value(where)};']
    return _with_writeback(addr, rt, f'ctx->r[{rt}] = {value("a")}')


def _store(insn, fc):
    helper = _STORES[insn.id]
    rt = _reg_num(insn, insn.operands[0].reg)
    if rt == PC:
        raise UnsupportedInstruction(insn, 'storing pc')
    addr = _address(insn)
    if addr.mode != 'offset' and rt == addr.rn:
        raise UnsupportedInstruction(insn, 'writeback to the stored register is unpredictable')
    if addr.mode == 'offset':
        where = _imm(addr.const) if addr.const is not None else addr.apply(addr.base)
        return [f'{helper}(ctx, {where}, ctx->r[{rt}]);']
    return _with_writeback(addr, rt, f'{helper}(ctx, a, ctx->r[{rt}])')


def _ldrd_strd(insn, fc):
    """ldrd/strd: two registers (rt, rt+1) from/to two consecutive words,
    with the same addressing modes as ldrh."""
    ops = insn.operands
    if len(ops) < 3 or ops[0].type != ARM_OP_REG or ops[1].type != ARM_OP_REG:
        raise UnsupportedInstruction(insn, 'unexpected operand shape')
    rt, rt2 = _reg_num(insn, ops[0].reg), _reg_num(insn, ops[1].reg)
    if rt % 2 or rt2 != rt + 1 or rt2 == PC:
        raise UnsupportedInstruction(insn, 'ldrd/strd needs an even/odd register pair below pc')
    addr = _address(insn, mem_index=2)
    if addr.mode != 'offset' and addr.rn in (rt, rt2):
        raise UnsupportedInstruction(insn, 'writeback to a transferred register is unpredictable')
    if insn.id == ARM_INS_LDRD:
        access = 'ctx->r[{t}] = read32(ctx, {a}); ctx->r[{t2}] = read32(ctx, {a} + 4u)'
    else:
        access = 'write32(ctx, {a}, ctx->r[{t}]); write32(ctx, {a} + 4u, ctx->r[{t2}])'
    if addr.mode == 'offset':
        # The address is computed once up front: ldrd may load over its own
        # base register.
        where = _imm(addr.const) if addr.const is not None else addr.apply(addr.base)
        parts = [part.strip() + ';' for part in access.format(t=rt, t2=rt2, a='a').split(';')]
        return ['{', f'    uint32_t a = {where};'] + [f'    {part}' for part in parts] + ['}']
    return _with_writeback(addr, rt, access.format(t=rt, t2=rt2, a='a'))


# ---------------------------------------------------------------- ldm / stm

# id: (load?, mode). push = stmdb sp!, pop = ldmia sp!.
_BLOCK = {
    ARM_INS_LDM: (True, 'ia'), ARM_INS_LDMIB: (True, 'ib'),
    ARM_INS_LDMDA: (True, 'da'), ARM_INS_LDMDB: (True, 'db'),
    ARM_INS_STM: (False, 'ia'), ARM_INS_STMIB: (False, 'ib'),
    ARM_INS_STMDA: (False, 'da'), ARM_INS_STMDB: (False, 'db'),
    ARM_INS_POP: (True, 'ia'), ARM_INS_PUSH: (False, 'db'),
}


def _block_addr(k):
    return 'b' if k == 0 else f'b + {k}u' if k > 0 else f'b - {-k}u'


def _block(insn, fc):
    """Block transfer: increment/decrement, after/before.

    ia: first word at base      ib: first word at base + 4
    da: last word at base       db: last word at base - 4
    The lowest-numbered register always uses the lowest address.
    """
    load, mode = _BLOCK[insn.id]
    ops = list(insn.operands)
    if insn.id in (ARM_INS_PUSH, ARM_INS_POP):
        rn, wback = SP, True
    else:
        rn, wback, ops = _reg_num(insn, ops[0].reg), insn.writeback, ops[1:]
    regs = sorted(_reg_num(insn, op.reg) for op in ops)
    n = len(regs)
    if _is_thumb(insn) and insn.id not in (ARM_INS_PUSH, ARM_INS_POP):
        # Thumb-1 ldmia/stmia have no W bit: they always write back, except
        # an ldmia whose base is in the list (then the loaded value wins).
        wback = not (load and rn in regs)

    if rn == PC:
        raise UnsupportedInstruction(insn, 'pc as the base register')
    if wback and rn in regs and (load or rn != regs[0]):
        # (A store is fine when the base is the lowest register in the list:
        # its original value is stored, e.g. libgcc's `push {sp, lr}`.)
        raise UnsupportedInstruction(insn, 'writeback with the base register in the list is unpredictable')
    if PC in regs and not load and _is_thumb(insn):
        raise UnsupportedInstruction(insn, 'storing pc')
    is_return = load and PC in regs
    if is_return and not (rn == SP and wback and mode == 'ia'):
        raise UnsupportedInstruction(insn, 'load into pc (indirect jumps come later)')

    start = {'ia': 0, 'ib': 4, 'da': -4 * n + 4, 'db': -4 * n}[mode]
    lines = ['{', f'    uint32_t b = ctx->r[{rn}];']
    for i, r in enumerate(regs):
        a = _block_addr(start + 4 * i)
        if not load and r == PC:
            # Storing pc stores this instruction + 8 (ARM11's choice).
            lines.append(f'    write32(ctx, {a}, {_imm(_pc(insn))});')
        elif not load:
            lines.append(f'    write32(ctx, {a}, ctx->r[{r}]);')
        elif r == PC:
            lines.append(f'    uint32_t pc = read32(ctx, {a});')
        else:
            lines.append(f'    ctx->r[{r}] = read32(ctx, {a});')
    if wback:
        lines.append(f'    ctx->r[{rn}] = {_block_addr(4 * n if mode in ("ia", "ib") else -4 * n)};')
    if is_return:
        lines.append('    RETURN_TO(pc);')
        fc.uses_ret = True
        fc.terminates = True
    lines.append('}')
    return lines


# ---------------------------------------------------------------- VFP (M5)
#
# VFPv2, ARM mode only (Thumb-1 has no VFP instructions). Registers are
# moved around as raw bits; all arithmetic goes through runtime/vfp.h,
# which implements ARM's NaN and conversion rules exactly.

def _vreg(insn, op):
    """('s', n) or ('d', n) for a VFP register operand."""
    if op.type != ARM_OP_REG:
        raise UnsupportedInstruction(insn, 'unexpected operand shape')
    name = insn.reg_name(op.reg)
    if name[0] in 'sd' and name[1:].isdigit():
        return name[0], int(name[1:])
    raise UnsupportedInstruction(insn, f'expected a VFP register, got {name}')


def _vref(kind, n):
    """C lvalue for a VFP register's raw bits."""
    return f'ctx->vfp.u[{n}]' if kind == 's' else f'ctx->vfp.x[{n}]'


def _vfp_regs(insn, count):
    ops = insn.operands
    if len(ops) != count:
        raise UnsupportedInstruction(insn, 'unexpected operand shape')
    regs = [_vreg(insn, op) for op in ops]
    if len({k for k, _ in regs}) != 1:
        raise UnsupportedInstruction(insn, 'mixed single/double operands')
    return regs


_VFP_BINARY = {ARM_INS_VADD: 'add', ARM_INS_VSUB: 'sub', ARM_INS_VMUL: 'mul', ARM_INS_VDIV: 'div'}
_VFP_UNARY = {ARM_INS_VSQRT: 'sqrt', ARM_INS_VNEG: 'neg', ARM_INS_VABS: 'abs'}


def _vfp_arith(insn, fc):
    if insn.id in _VFP_UNARY:
        (k, d), (_, m) = _vfp_regs(insn, 2)
        w = 32 if k == 's' else 64
        return [f'{_vref(k, d)} = vfp_{_VFP_UNARY[insn.id]}{w}({_vref(k, m)});']
    (k, d), (_, n), (_, m) = _vfp_regs(insn, 3)
    w = 32 if k == 's' else 64
    return [f'{_vref(k, d)} = vfp_{_VFP_BINARY[insn.id]}{w}({_vref(k, n)}, {_vref(k, m)});']


def _vfp_mac(insn, fc):
    """The multiply-accumulate family. On VFPv2 these are NOT fused: the
    product is rounded, then added, each step with its own NaN rules
    (ARM ARM: product = FPMul(n, m); result = FPAdd(+-d, +-product))."""
    (k, d), (_, n), (_, m) = _vfp_regs(insn, 3)
    w = 32 if k == 's' else 64
    dd, prod = _vref(k, d), f'vfp_mul{w}({_vref(k, n)}, {_vref(k, m)})'
    neg = f'vfp_neg{w}'
    expr = {
        ARM_INS_VMLA:  f'vfp_add{w}({dd}, {prod})',                # d + n*m
        ARM_INS_VMLS:  f'vfp_add{w}({dd}, {neg}({prod}))',         # d - n*m
        ARM_INS_VNMLA: f'vfp_add{w}({neg}({dd}), {neg}({prod}))',  # -d - n*m
        ARM_INS_VNMLS: f'vfp_add{w}({neg}({dd}), {prod})',         # -d + n*m
        ARM_INS_VNMUL: f'{neg}({prod})',                           # -(n*m)
    }[insn.id]
    return [f'{dd} = {expr};']


def _vfp_cmp(insn, fc):
    """vcmp/vcmpe set FPSCR's NZCV (vcmpe differs only in raising Invalid
    for quiet NaNs, and exception flags aren't modelled)."""
    ops = insn.operands
    k, a = _vreg(insn, ops[0])
    w = 32 if k == 's' else 64
    if len(ops) == 2 and ops[1].type == ARM_OP_IMM:            # compare with #0.0
        if ops[1].imm != 0:
            raise UnsupportedInstruction(insn, 'unexpected operand shape')
        b = '0u' if k == 's' else '0ull'
    else:
        (k2, bn) = _vreg(insn, ops[1])
        if k2 != k:
            raise UnsupportedInstruction(insn, 'mixed single/double operands')
        b = _vref(k, bn)
    return [f'vfp_cmp{w}(ctx, {_vref(k, a)}, {b});']


def _vmrs(insn, fc):
    # `vmrs APSR_nzcv, fpscr` (Capstone's id for it is the old name FMSTAT)
    # copies the VFP compare flags into the CPU flags.
    names = [insn.reg_name(op.reg) for op in insn.operands if op.type == ARM_OP_REG]
    if insn.id == ARM_INS_FMSTAT or names == ['apsr_nzcv', 'fpscr']:
        return ['vfp_flags_to_cpu(ctx);']
    raise UnsupportedInstruction(insn, 'reading the whole FPSCR (exception flags and modes are not modelled)')


_CVT = {    # (to, from) -> C expression template on the raw source bits {x}
    ('f64', 'f32'): 'vfp_f32_to_f64({x})',
    ('f32', 'f64'): 'vfp_f64_to_f32({x})',
    ('s32', 'f32'): 'vfp_to_s32((double)f32({x}))',
    ('u32', 'f32'): 'vfp_to_u32((double)f32({x}))',
    ('s32', 'f64'): 'vfp_to_s32(f64({x}))',
    ('u32', 'f64'): 'vfp_to_u32(f64({x}))',
    ('f32', 's32'): 'f32_bits((float)(int32_t){x})',
    ('f32', 'u32'): 'f32_bits((float){x})',
    ('f64', 's32'): 'f64_bits((double)(int32_t){x})',
    ('f64', 'u32'): 'f64_bits((double){x})',
}


def _vcvt(insn, fc):
    if insn.id == ARM_INS_VCVTR:
        # Rounds with the FPSCR rounding mode, which isn't modelled.
        raise UnsupportedInstruction(insn, 'vcvtr uses the FPSCR rounding mode (not modelled)')
    parts = insn.mnemonic.split('.')
    if len(parts) != 3 or (parts[1], parts[2]) not in _CVT:
        raise UnsupportedInstruction(insn, 'fixed-point or unexpected conversion')
    (kd, d), (km, m) = (_vreg(insn, op) for op in insn.operands)
    return [f'{_vref(kd, d)} = {_CVT[(parts[1], parts[2])].format(x=_vref(km, m))};']


def _vmov(insn, fc):
    """Every VFPv2 vmov is a plain bit copy (no NaN processing)."""
    ops = insn.operands
    if any(op.type != ARM_OP_REG for op in ops):
        raise UnsupportedInstruction(insn, 'vmov with an immediate is VFPv3, not on the 3DS')
    names = [insn.reg_name(op.reg) for op in ops]

    def kind(name):         # 's' / 'd' VFP register, 'r' core register (not pc)
        if name[0] in 'sd' and name[1:].isdigit():
            return name[0]
        return 'r' if _REGS.get(name, PC) != PC else '?'

    kinds = ''.join(kind(n) for n in names)
    if '?' in kinds:
        raise UnsupportedInstruction(insn, 'unexpected operand shape')

    def core(i):
        return _REGS[names[i]]

    def vfp(i):
        return int(names[i][1:])

    if kinds == 'rs':
        return [f'ctx->r[{core(0)}] = ctx->vfp.u[{vfp(1)}];']
    if kinds == 'sr':
        return [f'ctx->vfp.u[{vfp(0)}] = ctx->r[{core(1)}];']
    if kinds in ('ss', 'dd'):
        return [f'{_vref(kinds[0], vfp(0))} = {_vref(kinds[0], vfp(1))};']
    if kinds == 'rrd':      # d -> two core registers (low word first)
        return ['{', f'    uint64_t v = ctx->vfp.x[{vfp(2)}];',
                f'    ctx->r[{core(0)}] = (uint32_t)v;', f'    ctx->r[{core(1)}] = (uint32_t)(v >> 32);', '}']
    if kinds == 'drr':
        return [f'ctx->vfp.x[{vfp(0)}] = ctx->r[{core(1)}] | (uint64_t)ctx->r[{core(2)}] << 32;']
    raise UnsupportedInstruction(insn, 'unexpected operand shape')


def _vldr_vstr(insn, fc):
    k, n = _vreg(insn, insn.operands[0])
    addr = _address(insn)
    if addr.mode != 'offset':
        raise UnsupportedInstruction(insn, 'unexpected addressing mode')
    size = 4 if k == 's' else 8
    where = _imm(addr.const) if addr.const is not None else addr.apply(addr.base)
    reg = _vref(k, n)
    if insn.id == ARM_INS_VSTR:
        return [f'write{8 * size}(ctx, {where}, {reg});']
    if addr.const is not None and fc.program:
        data = fc.program.literal(addr.const, size)
        if data is not None:
            # A literal pool constant: fold it, and show its value.
            bits = int.from_bytes(data, 'little')
            value = struct.unpack('<f' if k == 's' else '<d', data)[0]
            suffix = 'u' if k == 's' else 'ull'
            return [f'{reg} = 0x{bits:0{2 * size}x}{suffix};  /* {value!r}, literal @{addr.const:08x} */']
    return [f'{reg} = read{8 * size}(ctx, {where});']


_VFP_BLOCK = {      # id: (load?, decrement-before?)
    ARM_INS_VLDMIA: (True, False), ARM_INS_VLDMDB: (True, True),
    ARM_INS_VSTMIA: (False, False), ARM_INS_VSTMDB: (False, True),
    ARM_INS_VPOP: (True, False), ARM_INS_VPUSH: (False, True),
}


def _vfp_block(insn, fc):
    """vldm/vstm (ia, db) and vpush (= vstmdb sp!) / vpop (= vldmia sp!)."""
    load, decrement = _VFP_BLOCK[insn.id]
    ops = list(insn.operands)
    if insn.id in (ARM_INS_VPUSH, ARM_INS_VPOP):
        rn, wback = SP, True
    else:
        rn, ops = _reg_num(insn, ops[0].reg), ops[1:]
        wback = bool((_encoding(insn) >> 21) & 1)       # the W bit
    if rn == PC:
        raise UnsupportedInstruction(insn, 'pc as the base register')
    regs = [_vreg(insn, op) for op in ops]
    k = regs[0][0]
    if any(rk != k for rk, _ in regs):
        raise UnsupportedInstruction(insn, 'mixed single/double registers')
    size = 4 if k == 's' else 8
    total = size * len(regs)
    start = -total if decrement else 0

    lines = ['{', f'    uint32_t b = ctx->r[{rn}];']
    for i, (_, n) in enumerate(regs):
        a = _block_addr(start + size * i)
        if load:
            lines.append(f'    {_vref(k, n)} = read{8 * size}(ctx, {a});')
        else:
            lines.append(f'    write{8 * size}(ctx, {a}, {_vref(k, n)});')
    if wback:
        lines.append(f'    ctx->r[{rn}] = {_block_addr(-total if decrement else total)};')
    lines.append('}')
    return lines


# ---------------------------------------------------------------- system (M6)

def _coproc(insn, fc):
    """mrc/mcr p15: decoded from the encoding fields, and only the
    operations user-mode 3DS code actually performs are accepted."""
    w = _encoding(insn)
    coproc, opc1, crn, rt = (w >> 8) & 0xF, (w >> 21) & 7, (w >> 16) & 0xF, (w >> 12) & 0xF
    opc2, crm, is_read = (w >> 5) & 7, w & 0xF, (w >> 20) & 1
    if coproc == 15 and opc1 == 0 and is_read and (crn, crm, opc2) == (13, 0, 3) and rt != PC:
        # TPIDRURO: the thread-local storage pointer (libctru keeps each
        # thread's IPC command buffer there).
        return [f'ctx->r[{rt}] = ctx->tls;']
    if coproc == 15 and opc1 == 0 and not is_read and (crn, crm) in ((7, 10), (7, 5)) and opc2 in (4, 5):
        # Data synchronisation / memory barrier, prefetch flush: no effect
        # on recompiled code running one guest thread at a time.
        return ['/* barrier: no-op */']
    raise UnsupportedInstruction(insn, f'coprocessor p{coproc} c{crn} c{crm} op {opc1}/{opc2}')


_EXCL_LOADS = {ARM_INS_LDREX: 'ldrex32', ARM_INS_LDREXH: 'ldrex16', ARM_INS_LDREXB: 'ldrex8'}
_EXCL_STORES = {ARM_INS_STREX: 'strex32', ARM_INS_STREXH: 'strex16', ARM_INS_STREXB: 'strex8'}


def _exclusive(insn, fc):
    """ldrex/strex (atomics): see the exclusive monitor in context.h."""
    ops = insn.operands
    mem = ops[-1]
    if mem.type != ARM_OP_MEM or mem.mem.index or mem.mem.disp:
        raise UnsupportedInstruction(insn, 'unexpected operand shape')
    base = _read_reg(insn, mem.mem.base)
    if insn.id in _EXCL_LOADS:
        rt = _dest_reg(insn, ops[0])
        return [f'ctx->r[{rt}] = {_EXCL_LOADS[insn.id]}(ctx, {base});']
    rd, rt = _dest_reg(insn, ops[0]), _reg_num(insn, ops[1].reg)
    if rd in (rt, _reg_num(insn, mem.mem.base)) or rt == PC:
        raise UnsupportedInstruction(insn, 'strex status register overlaps an operand (unpredictable)')
    return [f'ctx->r[{rd}] = {_EXCL_STORES[insn.id]}(ctx, {base}, ctx->r[{rt}]);  /* 0 = stored */']


def _clrex(insn, fc):
    return ['ctx->excl_valid = 0;']


def _svc(insn, fc):
    """A system call into the 3DS kernel: handed to the OS layer."""
    (op,) = insn.operands
    return [f'recomp_svc(ctx, {_imm(op.imm)});']


def _simd8(insn, fc):
    """uadd8 (sets the GE flags) and sel (uses them), as newlib's strcmp does."""
    ops = insn.operands
    if len(ops) != 3 or any(op.type != ARM_OP_REG for op in ops):
        raise UnsupportedInstruction(insn, 'unexpected operand shape')
    rd = _dest_reg(insn, ops[0])
    n, m = _read_reg(insn, ops[1].reg), _read_reg(insn, ops[2].reg)
    helper = 'arm_uadd8' if insn.id == ARM_INS_UADD8 else 'arm_sel'
    return [f'ctx->r[{rd}] = {helper}(ctx, {n}, {m});']


def _udf(insn, fc):
    fc.terminates = True
    return [f'recomp_udf(ctx, {_imm(insn.address)});  /* deliberate trap */']


def _nop(insn, fc):
    # In ARM mode Capstone reports nop as one of the HINT family
    # (nop/yield/wfe/wfi/sev). Only a true nop is harmless to drop.
    if insn.mnemonic != 'nop':
        raise UnsupportedInstruction(insn, 'hint instruction')
    return ['/* nop */']


# ---------------------------------------------------------------- dispatch

_HANDLERS = {
    **{op: _binary for op in _BINARY},
    **{op: _unary for op in _UNARY},
    **{op: _shift_mnemonic for op in _SHIFT_MNEMONICS},
    **{op: _extend for op in _EXTENDS},
    **{op: _reverse for op in _REVERSES},
    **{op: _long_mul for op in _LONG_MUL},
    ARM_INS_CLZ: _clz,
    ARM_INS_LDRD: _ldrd_strd,
    ARM_INS_STRD: _ldrd_strd,
    ARM_INS_B: _b,
    ARM_INS_BL: _bl,
    ARM_INS_BLX: _bl,
    ARM_INS_BX: _bx,
    ARM_INS_MUL: _mul,
    ARM_INS_MLA: _mul,
    ARM_INS_ADR: _adr,
    **{op: _load for op in _LOADS},
    **{op: _store for op in _STORES},
    **{op: _block for op in _BLOCK},
    ARM_INS_NOP: _nop,
    ARM_INS_MRC: _coproc,
    ARM_INS_MCR: _coproc,
    **{op: _exclusive for op in (*_EXCL_LOADS, *_EXCL_STORES)},
    ARM_INS_CLREX: _clrex,
    ARM_INS_SVC: _svc,
    ARM_INS_UADD8: _simd8,
    ARM_INS_SEL: _simd8,
    ARM_INS_UDF: _udf,
    ARM_INS_HINT: _nop,
    **{op: _vfp_arith for op in (*_VFP_BINARY, *_VFP_UNARY)},
    **{op: _vfp_mac for op in (ARM_INS_VMLA, ARM_INS_VMLS, ARM_INS_VNMLA, ARM_INS_VNMLS, ARM_INS_VNMUL)},
    ARM_INS_VCMP: _vfp_cmp,
    ARM_INS_VCMPE: _vfp_cmp,
    ARM_INS_VMRS: _vmrs,
    ARM_INS_FMSTAT: _vmrs,
    ARM_INS_VCVT: _vcvt,
    ARM_INS_VCVTR: _vcvt,
    ARM_INS_VMOV: _vmov,
    ARM_INS_VLDR: _vldr_vstr,
    ARM_INS_VSTR: _vldr_vstr,
    **{op: _vfp_block for op in _VFP_BLOCK},
}


def _wrap_cond(cond, lines):
    """Conditional execution: the instruction only happens if the condition holds."""
    if lines[0] == '{' and lines[-1] == '}':
        lines = [line[4:] for line in lines[1:-1]]
    if len(lines) == 1:
        return [f'if (cond_{cond}(ctx)) {{ {lines[0]} }}']
    return [f'if (cond_{cond}(ctx)) {{'] + [f'    {line}' for line in lines] + ['}']


def translate(insn, fc):
    """Return the C lines for one instruction."""
    handler = _HANDLERS.get(insn.id)
    if handler is None:
        raise UnsupportedInstruction(insn, 'not implemented')
    if insn.cc != ARM_CC_AL and insn.cc not in _CONDS:
        raise UnsupportedInstruction(insn, f'condition code {insn.cc}')

    fc.terminates = False
    lines = handler(insn, fc)
    if insn.cc != ARM_CC_AL:
        # Almost every ARM instruction has a 4-bit condition field.
        fc.terminates = False
        lines = _wrap_cond(_CONDS[insn.cc], lines)
    return lines


def _is_padding(insn):
    """The no-ops assemblers use for alignment: Thumb `mov r8, r8`, ARM
    `nop` and `mov r0, r0`."""
    w = _encoding(insn)
    return (w == 0x46c0 and insn.size == 2) or (insn.size == 4 and w in (0xe320f000, 0xe1a00000))


def _data_entries(fn, start, end):
    """Comment-only body entries for data inside a function (literal pools)."""
    out = []
    for a in range(start, end, 4):
        chunk = fn.code[a - fn.addr:min(a + 4, end) - fn.addr]
        if len(chunk) == 4:
            out.append((a, f'{a:08x}: literal pool .word 0x{int.from_bytes(chunk, "little"):08x}', []))
        else:
            out.append((a, f'{a:08x}: data {chunk.hex()}', []))
    return out


def translate_function(fn, insns, entries, program=None):
    """Translate a whole function into a TranslatedFunction."""
    if not insns:
        raise ValueError(f'{fn.name}: no code')
    fc = FunctionContext(fn, entries, program, {insn.address for insn in insns})

    body = []
    for i, insn in enumerate(insns):
        text = f'{insn.address:08x}: {insn.mnemonic} {insn.op_str}'.rstrip()
        after_terminator = fc.terminates and i > 0
        body.append((insn.address, text, translate(insn, fc)))
        if after_terminator and _is_padding(insn):
            # Alignment padding after a return/branch (before a literal
            # pool) is never executed; it doesn't make the code fall through.
            fc.terminates = True

        next_addr = insns[i + 1].address if i + 1 < len(insns) else fn.end
        if next_addr != insn.address + insn.size or i + 1 == len(insns):
            if not fc.terminates:
                # Without this check, the C function would quietly stop here
                # while the guest CPU would run on into whatever comes next.
                end = insn.address + insn.size
                where = 'falls off its end' if end == fn.end else f'runs into data at {end:08x}'
                raise UnsupportedInstruction(insn, f'{fn.name} {where} without returning')
            body += _data_entries(fn, insn.address + insn.size, next_addr)

    for target in fc.labels:
        if target not in fc.code_addrs:
            raise UnsupportedInstruction(insns[0], f'branch target {target:08x} is not an instruction start')

    return TranslatedFunction(fn, body, fc.labels, fc.uses_ret)
