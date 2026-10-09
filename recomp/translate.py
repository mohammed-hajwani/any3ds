"""Translate ARM instructions into C statements that operate on `Ctx *ctx`.

M0 subset: `add` (register or immediate operand 2, optional immediate shift)
and `bx lr`. Anything else is a hard error -- never skip an instruction.
"""

from capstone.arm import (
    ARM_CC_AL,
    ARM_INS_ADD,
    ARM_INS_BX,
    ARM_OP_IMM,
    ARM_OP_REG,
    ARM_SFT_ASR,
    ARM_SFT_ASR_REG,
    ARM_SFT_INVALID,
    ARM_SFT_LSL,
    ARM_SFT_LSR,
    ARM_SFT_ROR,
    ARM_SFT_RRX,
)


class UnsupportedInstruction(Exception):
    def __init__(self, insn, reason):
        self.addr = insn.address
        self.mnemonic = insn.mnemonic
        text = f'{insn.mnemonic} {insn.op_str}'.strip()
        super().__init__(f'{insn.address:08x}: unsupported instruction `{text}`: {reason}')


# Capstone prints r9-r12 by their APCS aliases.
_REGS = {f'r{i}': i for i in range(16)}
_REGS.update(sb=9, sl=10, fp=11, ip=12, sp=13, lr=14, pc=15)

PC, LR = 15, 14


def _reg_num(insn, reg):
    return _REGS[insn.reg_name(reg)]


def _read_reg(insn, reg):
    n = _reg_num(insn, reg)
    if n == PC:
        # In ARM mode, reading r15 gives the address of the current
        # instruction + 8 (a leftover of the original 3-stage pipeline).
        raise UnsupportedInstruction(insn, 'reads pc (pc-relative reads come later)')
    return f'ctx->r[{n}]'


def _imm(value):
    value &= 0xFFFFFFFF
    return f'{value}u' if value < 10 else f'0x{value:x}u'


def _shift_imm(insn, x, kind, n):
    """C expression for `x` shifted by the constant amount `n`.

    C leaves shifts by >= 32 undefined, so the ARM encodings that mean
    "shift by 32" (lsr #32, asr #32) are special-cased.
    """
    if kind == ARM_SFT_INVALID:          # no shift
        return x
    if kind == ARM_SFT_LSL and 0 <= n <= 31:
        return x if n == 0 else f'({x} << {n})'
    if kind == ARM_SFT_LSR and 1 <= n <= 32:
        return '0u' if n == 32 else f'({x} >> {n})'
    if kind == ARM_SFT_ASR and 1 <= n <= 32:
        # Shifting a 32-bit value right by 31 already fills every bit with
        # the sign, so asr #32 gives the same result as asr #31.
        return f'(uint32_t)((int32_t){x} >> {min(n, 31)})'
    if kind == ARM_SFT_ROR and 1 <= n <= 31:
        return f'(({x} >> {n}) | ({x} << {32 - n}))'
    if kind == ARM_SFT_RRX:
        # Rotate right by one *through* the carry flag: C goes into bit 31.
        return f'(({x} >> 1) | (ctx->c << 31))'
    if kind >= ARM_SFT_ASR_REG:
        raise UnsupportedInstruction(insn, 'shift amount in a register (M1)')
    raise UnsupportedInstruction(insn, f'shift type {kind} by {n}')


def _operand2(insn, op):
    """ARM's flexible second operand: an immediate, or a register through the barrel shifter."""
    if op.type == ARM_OP_IMM:
        # Capstone has already expanded the 8-bit-rotated encoding to its value.
        return _imm(op.imm)
    if op.type == ARM_OP_REG:
        return _shift_imm(insn, _read_reg(insn, op.reg), op.shift.type, op.shift.value)
    raise UnsupportedInstruction(insn, f'operand type {op.type}')


def _add(insn):
    ops = insn.operands
    if len(ops) == 2:                    # `add rd, op2` is short for `add rd, rd, op2`
        ops = [ops[0], ops[0], ops[1]]
    if len(ops) != 3 or ops[0].type != ARM_OP_REG or ops[1].type != ARM_OP_REG:
        raise UnsupportedInstruction(insn, 'unexpected operand shape')

    rd = _reg_num(insn, ops[0].reg)
    if rd == PC:
        raise UnsupportedInstruction(insn, 'writes pc (computed branches come later)')
    rn = _read_reg(insn, ops[1].reg)
    return [f'ctx->r[{rd}] = {rn} + {_operand2(insn, ops[2])};']


def _bx(insn):
    (op,) = insn.operands
    if op.type == ARM_OP_REG and _reg_num(insn, op.reg) == LR:
        return ['return;']
    raise UnsupportedInstruction(insn, 'bx to a register other than lr (M1/M4)')


_HANDLERS = {
    ARM_INS_ADD: _add,
    ARM_INS_BX: _bx,
}


def translate(insn):
    """Return the list of C statements for one instruction."""
    if insn.cc != ARM_CC_AL:
        # Almost every ARM instruction has a 4-bit condition field; M0 only
        # accepts AL ("always").
        raise UnsupportedInstruction(insn, 'conditional execution (M1)')
    if insn.update_flags:
        raise UnsupportedInstruction(insn, 'sets condition flags (M1)')
    handler = _HANDLERS.get(insn.id)
    if handler is None:
        raise UnsupportedInstruction(insn, 'not implemented')
    return handler(insn)


def translate_function(fn, insns):
    """Translate a whole function: a list of (insn, [C statements])."""
    body = [(insn, translate(insn)) for insn in insns]
    if body[-1][1][-1:] != ['return;']:
        # Without this check, the C function would quietly return here
        # while the guest code would have run on into whatever comes next.
        raise UnsupportedInstruction(insns[-1], f'{fn.name} falls off its end without returning')
    return body
