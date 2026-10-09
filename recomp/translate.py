"""Translate ARM instructions into C statements that operate on `Ctx *ctx`.

Supported (M1): data processing (mov mvn add adc sub sbc rsb and orr eor bic
cmp cmn tst teq, with and without S, immediate shifts), the lsl/lsr/asr/
ror/rrx shift mnemonics, uxtb, b/bl/bx lr with any condition, push/pop.

Anything else is a hard error -- never skip an instruction.
"""

from dataclasses import dataclass, field

from capstone.arm import (
    ARM_CC_AL, ARM_CC_EQ, ARM_CC_GE, ARM_CC_GT, ARM_CC_HI, ARM_CC_HS,
    ARM_CC_LE, ARM_CC_LO, ARM_CC_LS, ARM_CC_LT, ARM_CC_MI, ARM_CC_NE,
    ARM_CC_PL, ARM_CC_VC, ARM_CC_VS,
    ARM_INS_ADC, ARM_INS_ADD, ARM_INS_AND, ARM_INS_ASR, ARM_INS_B,
    ARM_INS_BIC, ARM_INS_BL, ARM_INS_BX, ARM_INS_CMN, ARM_INS_CMP,
    ARM_INS_EOR, ARM_INS_LSL, ARM_INS_LSR, ARM_INS_MOV, ARM_INS_MVN,
    ARM_INS_ORR, ARM_INS_POP, ARM_INS_PUSH, ARM_INS_ROR, ARM_INS_RRX,
    ARM_INS_RSB, ARM_INS_SBC, ARM_INS_SUB, ARM_INS_TEQ, ARM_INS_TST,
    ARM_INS_UXTB,
    ARM_OP_IMM, ARM_OP_REG,
    ARM_SFT_ASR, ARM_SFT_ASR_REG, ARM_SFT_INVALID, ARM_SFT_LSL,
    ARM_SFT_LSR, ARM_SFT_ROR, ARM_SFT_RRX,
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
    body: list                          # [(insn, [C lines])]
    labels: set
    uses_ret: bool


# ---------------------------------------------------------------- operands

def _reg_num(insn, reg):
    return _REGS[insn.reg_name(reg)]


def _imm(value):
    value &= 0xFFFFFFFF
    return f'{value}u' if value < 10 else f'0x{value:x}u'


def _read_reg(insn, reg):
    n = _reg_num(insn, reg)
    if n == PC:
        # In ARM mode, reading r15 gives the address of the current
        # instruction + 8 (a leftover of the original 3-stage pipeline).
        return _imm(insn.address + 8)
    return f'ctx->r[{n}]'


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
}

# id: (C expression, writes Rd, flag kind)
_BINARY = {
    ARM_INS_ADD: ('{n} + {m}', True, 'add'),
    ARM_INS_ADC: ('{n} + {m} + ctx->c', True, 'adc'),
    ARM_INS_SUB: ('{n} - {m}', True, 'sub'),
    ARM_INS_SBC: ('{n} - {m} - (1u - ctx->c)', True, 'sbc'),
    ARM_INS_RSB: ('{m} - {n}', True, 'rsb'),
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

    n = _read_reg(insn, rn_op.reg)
    m, carry = _operand2(insn, op2)
    expr = template.format(n=n, m=m) if template else None
    setflags = insn.update_flags or not writes_rd
    return _emit_dp(rd, expr, kind, setflags, carry, n, m)


def _unary(insn, fc):
    ops = insn.operands
    if len(ops) != 2:
        raise UnsupportedInstruction(insn, 'unexpected operand shape')
    rd = _dest_reg(insn, ops[0])
    m, carry = _operand2(insn, ops[1])
    return _emit_dp(rd, _UNARY[insn.id].format(m=m), 'logic', insn.update_flags, carry)


def _shift_mnemonic(insn, fc):
    """`lsrs r3, r3, #1` etc.: Capstone's name for mov with a shifted operand."""
    # Capstone attaches an immediate shift to the source operand, e.g.
    # `lsrs r3, r3, #1` has operands [r3, r3 (lsr 1)]. A third operand
    # means the amount comes from a register.
    ops = insn.operands
    if len(ops) == 3:
        raise UnsupportedInstruction(insn, 'shift amount in a register')
    if len(ops) != 2 or ops[1].type != ARM_OP_REG:
        raise UnsupportedInstruction(insn, 'unexpected operand shape')
    rd = _dest_reg(insn, ops[0])
    x = _read_reg(insn, ops[1].reg)
    if insn.id == ARM_INS_RRX:          # carries no shift info of its own
        m, carry = _shift_imm(insn, x, ARM_SFT_RRX, 1)
    else:
        if ops[1].shift.type != _SHIFT_MNEMONICS[insn.id]:
            raise UnsupportedInstruction(insn, 'unexpected operand shape')
        m, carry = _shift_imm(insn, x, ops[1].shift.type, ops[1].shift.value)
    return _emit_dp(rd, m, 'logic', insn.update_flags, carry)


def _uxtb(insn, fc):
    """Zero-extend the low byte, after an optional rotation by 8/16/24."""
    ops = insn.operands
    if len(ops) != 2 or ops[1].type != ARM_OP_REG:
        raise UnsupportedInstruction(insn, 'unexpected operand shape')
    rd = _dest_reg(insn, ops[0])
    x, _ = _shift_imm(insn, _read_reg(insn, ops[1].reg), ops[1].shift.type, ops[1].shift.value)
    return [f'ctx->r[{rd}] = {x} & 0xffu;']


# ---------------------------------------------------------------- control flow

def _branch_target(insn):
    (op,) = insn.operands
    if op.type != ARM_OP_IMM:
        raise UnsupportedInstruction(insn, 'branch to a register')
    return op.imm & 0xFFFFFFFF


def _b(insn, fc):
    target = _branch_target(insn)
    if not fc.start <= target < fc.end:
        raise UnsupportedInstruction(
            insn, f'branch to {target:08x} leaves {fc.fn.name} (tail calls come later)')
    fc.labels.add(target)
    fc.terminates = True
    return [f'goto L_{target:08x};']


def _bl(insn, fc):
    target = _branch_target(insn)
    callee = fc.entries.get(target)
    if callee is None:
        raise UnsupportedInstruction(
            insn, f'bl target {target:08x} is not the start of a known function')
    # The callee sees the real return address in lr, exactly as on hardware.
    return [f'ctx->r[14] = {_imm(insn.address + 4)};', f'{c_name(target)}(ctx);  /* {callee.name} */']


def _bx(insn, fc):
    (op,) = insn.operands
    if op.type == ARM_OP_REG and _reg_num(insn, op.reg) == LR:
        fc.uses_ret = True
        fc.terminates = True
        return ['RETURN_TO(ctx->r[14]);']
    raise UnsupportedInstruction(insn, 'bx to a register other than lr (M4)')


# ---------------------------------------------------------------- push / pop

def _reglist(insn):
    regs = sorted(_reg_num(insn, op.reg) for op in insn.operands)
    if SP in regs:
        raise UnsupportedInstruction(insn, 'sp in the register list is unpredictable')
    return regs


def _push(insn, fc):
    # stmdb sp!: the lowest-numbered register goes at the lowest address.
    regs = _reglist(insn)
    if PC in regs:
        raise UnsupportedInstruction(insn, 'pushing pc')
    lines = ['{', f'    uint32_t sp = ctx->r[13] - {4 * len(regs)}u;']
    lines += [f'    write32(ctx, sp + {4 * i}u, ctx->r[{r}]);' for i, r in enumerate(regs)]
    lines += ['    ctx->r[13] = sp;', '}']
    return lines


def _pop(insn, fc):
    # ldmia sp!. Popping into pc is how a function that saved lr returns.
    regs = _reglist(insn)
    lines = ['{', '    uint32_t sp = ctx->r[13];']
    for i, r in enumerate(regs):
        if r == PC:
            lines.append(f'    uint32_t pc = read32(ctx, sp + {4 * i}u);')
        else:
            lines.append(f'    ctx->r[{r}] = read32(ctx, sp + {4 * i}u);')
    lines.append(f'    ctx->r[13] = sp + {4 * len(regs)}u;')
    if PC in regs:
        lines.append('    RETURN_TO(pc);')
        fc.uses_ret = True
        fc.terminates = True
    lines.append('}')
    return lines


# ---------------------------------------------------------------- dispatch

_HANDLERS = {
    **{op: _binary for op in _BINARY},
    **{op: _unary for op in _UNARY},
    **{op: _shift_mnemonic for op in _SHIFT_MNEMONICS},
    ARM_INS_UXTB: _uxtb,
    ARM_INS_B: _b,
    ARM_INS_BL: _bl,
    ARM_INS_BX: _bx,
    ARM_INS_PUSH: _push,
    ARM_INS_POP: _pop,
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


def translate_function(fn, insns, entries):
    """Translate a whole function into a TranslatedFunction."""
    fc = FunctionContext(fn, entries)
    body = [(insn, translate(insn, fc)) for insn in insns]

    if not fc.terminates:
        # Without this check, the C function would quietly return here
        # while the guest code would have run on into whatever comes next.
        raise UnsupportedInstruction(insns[-1], f'{fn.name} falls off its end without returning')

    starts = {insn.address for insn in insns}
    for target in fc.labels:
        if target not in starts:
            raise UnsupportedInstruction(insns[0], f'branch target {target:08x} is not an instruction start')

    return TranslatedFunction(fn, body, fc.labels, fc.uses_ret)
