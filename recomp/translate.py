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

from dataclasses import dataclass, field

from capstone.arm import (
    ARM_CC_AL, ARM_CC_EQ, ARM_CC_GE, ARM_CC_GT, ARM_CC_HI, ARM_CC_HS,
    ARM_CC_LE, ARM_CC_LO, ARM_CC_LS, ARM_CC_LT, ARM_CC_MI, ARM_CC_NE,
    ARM_CC_PL, ARM_CC_VC, ARM_CC_VS,
    ARM_INS_ADC, ARM_INS_ADD, ARM_INS_AND, ARM_INS_ASR, ARM_INS_B,
    ARM_INS_BIC, ARM_INS_BL, ARM_INS_BX, ARM_INS_CMN, ARM_INS_CMP,
    ARM_INS_EOR, ARM_INS_HINT, ARM_INS_LDM, ARM_INS_LDMDA, ARM_INS_LDMDB, ARM_INS_LDMIB,
    ARM_INS_LDR, ARM_INS_LDRB, ARM_INS_LDRH, ARM_INS_LDRSB, ARM_INS_LDRSH,
    ARM_INS_LSL, ARM_INS_LSR, ARM_INS_MOV, ARM_INS_MVN, ARM_INS_NOP,
    ARM_INS_ORR, ARM_INS_POP, ARM_INS_PUSH, ARM_INS_ROR, ARM_INS_RRX,
    ARM_INS_RSB, ARM_INS_SBC, ARM_INS_STM, ARM_INS_STMDA, ARM_INS_STMDB,
    ARM_INS_STMIB, ARM_INS_STR, ARM_INS_STRB, ARM_INS_STRH, ARM_INS_SUB,
    ARM_INS_TEQ, ARM_INS_TST, ARM_INS_UXTB,
    ARM_OP_IMM, ARM_OP_MEM, ARM_OP_REG,
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


def _jump_table(insn, fc):
    """gcc's switch dispatch: `add pc, pc, rX, lsl #2`.

    pc reads as this instruction + 8, so this jumps to addr + 8 + 4*rX,
    into a row of branches/returns that follows. Becomes a C switch over
    every code address from there to the end of the function.
    """
    _, rn_op, op2 = insn.operands
    if (insn.update_flags or rn_op.type != ARM_OP_REG or _reg_num(insn, rn_op.reg) != PC
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


def _address(insn):
    ops = insn.operands
    if len(ops) < 2 or ops[1].type != ARM_OP_MEM:
        raise UnsupportedInstruction(insn, 'unexpected operand shape')
    mem = ops[1]
    rn = _reg_num(insn, mem.mem.base)
    addr = _Addr(rn, _read_reg(insn, mem.mem.base))

    # Bit 24 (P) of the encoding says pre/offset (1) or post-index (0), for
    # both the word/byte and halfword/signed-byte encodings. Don't trust
    # Capstone's post_index flag: it's always False for ldrh/strh/ldrsb/ldrsh.
    post = not (int.from_bytes(insn.bytes, 'little') >> 24) & 1
    if post != (len(ops) == 3):
        raise UnsupportedInstruction(insn, 'unexpected operand shape')

    if post:
        # Capstone gives the post-index offset as a third operand.
        addr.offset, addr.mode = _offset(insn, ops[2], ops[2].subtracted), 'post'
    elif mem.mem.index:
        x, _ = _shift_imm(insn, _read_reg(insn, mem.mem.index), mem.shift.type, mem.shift.value)
        addr.offset = ('-' if mem.subtracted else '+', x)
        addr.mode = 'pre' if insn.writeback else 'offset'
    else:
        disp = mem.mem.disp
        addr.offset = None if disp == 0 else ('-' if disp < 0 else '+', _imm(abs(disp)))
        addr.mode = 'pre' if insn.writeback else 'offset'
        if rn == PC:
            # pc-relative: pc reads as this instruction + 8, so the address
            # is a constant we can compute now.
            addr.const = (insn.address + 8 + disp) & 0xFFFFFFFF

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

    if rn == PC:
        raise UnsupportedInstruction(insn, 'pc as the base register')
    if wback and rn in regs:
        raise UnsupportedInstruction(insn, 'writeback with the base register in the list is unpredictable')
    if PC in regs and not load:
        raise UnsupportedInstruction(insn, 'storing pc')
    is_return = load and PC in regs
    if is_return and not (rn == SP and wback and mode == 'ia'):
        raise UnsupportedInstruction(insn, 'load into pc (indirect jumps come later)')

    start = {'ia': 0, 'ib': 4, 'da': -4 * n + 4, 'db': -4 * n}[mode]
    lines = ['{', f'    uint32_t b = ctx->r[{rn}];']
    for i, r in enumerate(regs):
        a = _block_addr(start + 4 * i)
        if not load:
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
    ARM_INS_UXTB: _uxtb,
    ARM_INS_B: _b,
    ARM_INS_BL: _bl,
    ARM_INS_BX: _bx,
    **{op: _load for op in _LOADS},
    **{op: _store for op in _STORES},
    **{op: _block for op in _BLOCK},
    ARM_INS_NOP: _nop,
    ARM_INS_HINT: _nop,
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
        body.append((insn.address, text, translate(insn, fc)))

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
