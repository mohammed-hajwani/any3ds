"""Find the functions of a program that has no symbols (a retail code.bin,
or a homebrew ELF with its symbols ignored).

With symbols, the ELF tells us where every function starts, whether it is
ARM or Thumb, and (mapping symbols $a/$t/$d) which bytes are code and which
are literal-pool data. Without them we follow the code itself:

1. Recursive descent from the entry point. Inside a function we follow
   fall-through and branches; `bl`/`blx #imm` targets are new functions
   (`blx` switches ARM <-> Thumb). Only instructions actually reached count
   as code, so literal pools are never decoded.
2. A `b` to a known function start is a tail call. Any other forward `b`
   stays inside the function, however far it goes (gcc moves cold blocks
   past other functions); a `b` back below the function's own start
   becomes a function of its own (a tail call into it). Falling through
   into another function's start is a tail call too. So a function's code
   can have gaps holding other functions, and the same code can belong to
   two functions; it is then simply recompiled twice.
3. Words read by pc-relative loads are literal-pool data (so the translator
   can fold them as constants).
4. Function pointers (vtables, callbacks, thread entry points) are never
   called directly. Every word in the data segments or a literal pool that
   points into the code is a candidate; a candidate is kept only if all of
   its code decodes. A rejected candidate is listed in the report, never
   compiled.

All of this runs to a fixpoint: a new function start can end another
function's extent, and new literal data can stop a fall-through after a
call that never returns.
"""

from bisect import bisect_right
from dataclasses import dataclass, field, replace

from capstone.arm import (
    ARM_CC_AL, ARM_CC_LO, ARM_CC_LS, ARM_INS_ADD, ARM_INS_B, ARM_INS_BL, ARM_INS_BLX, ARM_INS_BX, ARM_INS_CMP,
    ARM_INS_CMN, ARM_INS_TEQ, ARM_INS_TST, ARM_INS_ADR,
    ARM_INS_LDM, ARM_INS_LDMDA, ARM_INS_LDMDB, ARM_INS_LDMIB, ARM_INS_LDR, ARM_INS_LDRB, ARM_INS_LDRD,
    ARM_INS_LDRH, ARM_INS_LDRSB, ARM_INS_LDRSH, ARM_INS_LSL, ARM_INS_MOV, ARM_INS_POP, ARM_INS_UDF, ARM_INS_VLDR,
    ARM_OP_IMM, ARM_OP_MEM, ARM_OP_REG,
)

from .disasm import DisasmError, _engine
from .elf_loader import Function, LoadError
from .translate import _REGS, PC, UnsupportedInstruction, _address, translate_function


def _reg_num(insn, reg):
    """A core register's number (r0-r15), or None for VFP and other registers."""
    return _REGS.get(insn.reg_name(reg))

_LOAD_SIZES = {ARM_INS_LDR: 4, ARM_INS_LDRB: 1, ARM_INS_LDRH: 2, ARM_INS_LDRSB: 1, ARM_INS_LDRSH: 2,
               ARM_INS_LDRD: 8}
_LDM = (ARM_INS_LDM, ARM_INS_LDMDA, ARM_INS_LDMDB, ARM_INS_LDMIB, ARM_INS_POP)


class Rejected(Exception):
    """A speculative function start turned out not to be code. `root` is
    the start whose exploration reached the bad code."""
    root = None


@dataclass
class Explored:
    """One function, as recursive descent found it."""
    addr: int
    thumb: bool
    insns: dict = field(default_factory=dict)       # address -> capstone insn
    calls: set = field(default_factory=set)         # (target, thumb) of bl/blx/tail calls
    call_sites: set = field(default_factory=set)    # (target, thumb, instruction address)
    data: set = field(default_factory=set)          # (address, size) of literal-pool reads
    pointers: set = field(default_factory=set)      # literal values that point into code
    root: tuple = None                              # the start whose exploration found this one
    falls_into: set = field(default_factory=set)    # other function starts this code runs straight into
    deps: dict = field(default_factory=dict)        # address -> (is a start, is data) as seen while exploring


class Discovery:
    def __init__(self, program):
        self.program = program
        # A CRO module has no entry point; it names its text segment instead.
        code = [program.text_segment] if getattr(program, 'text_segment', None) else \
            [s for s in program.segments if not s.writable and s.vaddr == program.entry & ~1]
        if not code:
            raise LoadError('no code segment at the entry point')
        self.text = code[0]
        self.text_end = self.text.vaddr + len(self.text.data)
        self._cache = {}
        self._explored = {}         # (addr, thumb) -> Explored, reused while its deps still hold
        self._translatable = {}     # function shape -> whether the translator accepts it
        self.unresolved = set()     # jump tables whose targets we couldn't find
        self.jump_tables = {}       # address of a `ldr pc` switch -> its targets, for the translator
        program.jump_tables = self.jump_tables
        self._added = None          # starts added while trying a guess, to undo if it fails
        self._start_info = {}       # start -> (thumb, root) when it was last explored

    # ------------------------------------------------------------ decoding

    def in_text(self, addr):
        return self.text.vaddr <= addr < self.text_end

    def decode(self, addr, thumb):
        """The instruction at addr, or None if those bytes aren't one we accept."""
        key = (addr, thumb)
        if key in self._cache:
            return self._cache[key]
        insn = None
        if self.in_text(addr) and addr % (2 if thumb else 4) == 0:
            off = addr - self.text.vaddr
            raw = self.text.data[off:off + 4]
            try:
                got = next(_engine(thumb).disasm(raw, addr, 1), None)
            except Exception:       # noqa: BLE001 (capstone raises for some encodings)
                got = None
            if got is not None and (not thumb or got.size == 2 or got.id in (ARM_INS_BL, ARM_INS_BLX)):
                insn = got          # (32-bit Thumb other than bl/blx is Thumb-2: not ARMv6K)
        self._cache[key] = insn
        return insn

    # ------------------------------------------------------------ one function

    def _status(self, fn, a):
        """(is a function start, is literal data) for address a, remembered
        in fn.deps: the result of exploring fn depends only on these."""
        st = (a in self.starts_set, a in self._cur_data or a in self._cur_own)
        fn.deps[a] = st
        return st

    def explore(self, addr, thumb, starts, data_words, own=frozenset()):
        """Recursive descent through one function. `starts` is the sorted
        list of known function starts, `data_words` the known literal data.
        Raises Rejected if code it reaches can't be decoded.

        Discovery explores the same functions over and over as it learns
        more; a function is only explored again if one of the addresses it
        looked at changed status (became a function start, or data)."""
        key = (addr, thumb)
        hit = self._explored.get(key)
        if hit is not None:
            starts_set = self.starts_set
            if all((a in starts_set, a in data_words or a in own) == st for a, st in hit.deps.items()):
                return hit
        self._cur_data, self._cur_own = data_words, own
        fn = Explored(addr, thumb)
        limit = self.text_end
        work = [(addr, False)]
        while work:
            a, after_call = work.pop()
            if a in fn.insns:
                continue
            is_start, is_data = self._status(fn, a)
            if a != addr and is_start:
                fn.falls_into.add(a)
                continue            # runs on into another function: a tail call (the translator's job)
            if a >= limit:
                if after_call:
                    continue
                raise Rejected(f'{a:08x}: code runs off the end of the code segment')
            if is_data:
                if after_call:
                    continue        # a call that never returns, followed by data
                raise Rejected(f'{a:08x}: code runs into literal data')
            insn = self.decode(a, thumb)
            if insn is None:
                if after_call:
                    continue        # a call that never returns, followed by something that isn't code
                raise Rejected(f'{a:08x}: cannot decode an instruction here')
            fn.insns[a] = insn
            nxt = a + insn.size
            self._analyse(fn, insn, thumb, work, nxt, limit)
        self._explored[key] = fn
        return fn

    def _analyse(self, fn, insn, thumb, work, nxt, limit):
        cond = insn.cc != ARM_CC_AL
        iid = insn.id
        ops = insn.operands

        if iid in (ARM_INS_BL, ARM_INS_BLX) and len(ops) == 1 and ops[0].type == ARM_OP_IMM:
            target = ops[0].imm & 0xFFFFFFFF
            to_thumb = (not thumb) if iid == ARM_INS_BLX else thumb
            if iid == ARM_INS_BLX and thumb:
                target &= ~3
            self._call(fn, target, to_thumb, insn)
            work.append((nxt, True))
            return

        if iid == ARM_INS_B and len(ops) == 1 and ops[0].type == ARM_OP_IMM:
            target = ops[0].imm & 0xFFFFFFFF
            if self._status(fn, target)[0] and target != fn.addr:
                self._call(fn, target, thumb, insn)         # a tail call
            elif fn.addr <= target < limit:
                work.append((target, False))                # inside this function
            else:
                self._call(fn, target, thumb, insn)         # back below our start: tail call into new code
            if cond:
                work.append((nxt, False))
            return

        if iid in (ARM_INS_BLX, ARM_INS_BX):                # register forms: indirect call / return / tail call
            if iid == ARM_INS_BLX or cond:
                work.append((nxt, iid == ARM_INS_BLX))
            return

        if iid in _LDM and any(op.type == ARM_OP_REG and _reg_num(insn, op.reg) == PC for op in ops):
            if cond:
                work.append((nxt, False))
            return                  # pop {.., pc}: a return

        if iid == ARM_INS_UDF:
            return

        writes_pc = bool(ops) and ops[0].type == ARM_OP_REG and _reg_num(insn, ops[0].reg) == PC
        if iid in _LOAD_SIZES or iid == ARM_INS_VLDR:
            self._literal(fn, insn, iid)
        if iid in (ARM_INS_ADD, ARM_INS_ADR) and not writes_pc:
            self._pc_relative_address(fn, insn, thumb)
        if writes_pc:
            if iid == ARM_INS_ADD and not thumb:
                self._jump_table(fn, insn, work, nxt, limit)
                return
            if iid == ARM_INS_LDR and not thumb and self._address_table(fn, insn, work, nxt):
                return
            if iid == ARM_INS_LDR and self._veneer(fn, insn):
                if cond:
                    work.append((nxt, False))
                return
            if cond:
                work.append((nxt, False))
            return                  # mov pc, lr / ldr pc, [sp], #4 / other computed jumps
        work.append((nxt, False))

    def _call(self, fn, target, thumb, insn):
        if not self.in_text(target):
            raise Rejected(f'{insn.address:08x}: {insn.mnemonic} to {target:08x}, outside the code')
        fn.calls.add((target, thumb))
        fn.call_sites.add((target, thumb, insn.address))

    def _literal(self, fn, insn, iid):
        """A pc-relative load reads literal-pool data."""
        mem = next((op for op in insn.operands if op.type == ARM_OP_MEM), None)
        if mem is None or _reg_num(insn, mem.mem.base) != PC:
            return
        try:
            addr = _address(insn, 2 if iid == ARM_INS_LDRD else 1).const
        except UnsupportedInstruction:
            return
        if addr is None:
            return
        if iid == ARM_INS_VLDR:
            size = 8 if insn.reg_name(insn.operands[0].reg).startswith('d') else 4
        else:
            size = _LOAD_SIZES[iid]
        if not self.in_text(addr):
            return
        fn.data.add((addr, size))
        if size == 4:
            off = addr - self.text.vaddr
            value = int.from_bytes(self.text.data[off:off + 4], 'little')
            if self.in_text(value & ~1):
                fn.pointers.add(value)

    def _jump_table(self, fn, insn, work, nxt, limit):
        """`add pc, pc, rX, lsl #2`: jumps to insn + 8 + 4*rX, a row of
        branches. The row runs while the words are unconditional `b`s (and,
        when a `cmp rX, #N` bounds the index, for N + 1 entries)."""
        ops = insn.operands
        if not (len(ops) == 3 and ops[1].type == ARM_OP_REG and _reg_num(insn, ops[1].reg) == PC
                and ops[2].type == ARM_OP_REG):
            return                  # some other computed jump: the translator will say so
        index = _reg_num(insn, ops[2].reg)
        bound = None
        cmp = self._bound_check(fn, insn, index)
        if cmp is not None and insn.cc == ARM_CC_LS:
            bound = cmp + 1
        if insn.cc != ARM_CC_AL:
            work.append((nxt, False))
        offsets = self._offset_table(fn, insn, index)
        if offsets is not None:
            # gcc -O2's compact switch: `ldrsb rX, [table, i]` then
            # `add pc, pc, rX, lsl #2`: the offsets live in a data table.
            for e in offsets:
                target = insn.address + 8 + 4 * e
                if fn.addr <= target < limit:
                    work.append((target, False))
            return
        first = self.decode(insn.address + 8, False)
        if bound is None and (first is None or first.id != ARM_INS_B or first.cc != ARM_CC_AL):
            self.unresolved.add(insn.address)       # neither a row of branches nor a table we can read
            return
        a, n = insn.address + 8, 0
        while a < limit and not self._status(fn, a)[0] and (bound is None or n < bound):
            entry = self.decode(a, False)
            if entry is None or entry.id != ARM_INS_B or entry.cc != ARM_CC_AL:
                if bound is None:
                    break
            work.append((a, False))
            a, n = a + 4, n + 1

    def _pc_relative_address(self, fn, insn, thumb):
        """`add rX, pc, #n` / `adr rX, label`: an address next to the code.
        If it points at code, it may be called: ARMCC's interworking veneer
        `add ip, pc, #1; bx ip` enters the Thumb function right after it."""
        ops = insn.operands
        regs = [_reg_num(insn, op.reg) for op in ops if op.type == ARM_OP_REG]
        if insn.id == ARM_INS_ADD and PC in regs[1:] and len(regs) == len(ops) and len(regs) >= 2:
            # Position-independent code: `ldr rX, =offset` then `add rX, pc`
            # (Thumb) or `add rX, pc, rX` (ARM): the address is the literal
            # plus pc, here.
            other = [r for r in regs[1:] if r != PC] or [regs[0]]
            load = next((p for p in self._back(fn, insn.address, 8) if self._writes(p, other[0])), None)
            mem = load.operands[1] if load is not None and len(load.operands) == 2 else None
            if load is None or load.id != ARM_INS_LDR or mem.type != ARM_OP_MEM or \
                    _reg_num(load, mem.mem.base) != PC:
                return
            raw = self.program.read(_address(load).const, 4)
            if raw is None:
                return
            value = (int.from_bytes(raw, 'little') + insn.address + (4 if thumb else 8)) & 0xFFFFFFFF
            if self.in_text(value & ~1):
                fn.pointers.add(value)
            return
        if insn.id == ARM_INS_ADR:          # Capstone's Thumb form: the offset from Align(pc, 4)
            if len(ops) != 2 or ops[1].type != ARM_OP_IMM:
                return
            imm = ops[1].imm
        else:                               # ARM: add rX, pc, #n
            if len(ops) != 3 or ops[1].type != ARM_OP_REG or _reg_num(insn, ops[1].reg) != PC \
                    or ops[2].type != ARM_OP_IMM:
                return
            imm = ops[2].imm
        base = (insn.address + 4) & ~3 if thumb else insn.address + 8
        value = (base + imm) & 0xFFFFFFFF
        if self.in_text(value & ~1):
            fn.pointers.add(value)

    def _bound_check(self, fn, insn, index):
        """N from the `cmp rIndex, #N` whose flags insn's condition uses:
        compilers schedule other instructions in between (ARMCC puts a
        `push` or `strb` there), so look back past any that leave both the
        index register and the flags alone."""
        for p in self._back(fn, insn.address, 8):
            if p.id == ARM_INS_CMP:
                ops = p.operands
                if ops[1].type == ARM_OP_IMM and _reg_num(p, ops[0].reg) == index:
                    return ops[1].imm
                return None
            if self._writes(p, index) or p.update_flags or p.id in (ARM_INS_CMN, ARM_INS_TST, ARM_INS_TEQ):
                return None
            if p.id in (ARM_INS_B, ARM_INS_BL, ARM_INS_BLX, ARM_INS_BX):
                return None
        return None

    def _veneer(self, fn, insn):
        """`ldr pc, [pc, #-4]` and a word: a jump to that absolute address
        (linker veneers for far branches). Bit 0 of the word selects Thumb.
        Returns False if insn isn't a pc-relative literal load."""
        mem = insn.operands[1]
        if mem.type != ARM_OP_MEM or _reg_num(insn, mem.mem.base) != PC or mem.mem.index:
            return False
        raw = self.program.read(_address(insn).const, 4)
        if raw is None:
            return False
        target = int.from_bytes(raw, 'little')
        if self.in_text(target & ~1):
            self._call(fn, target & ~1, bool(target & 1), insn)
        else:
            self.unresolved.add(insn.address)       # e.g. into a CRO module: not ours
        return True

    def _address_table(self, fn, insn, work, nxt):
        """ARMCC's switch: `cmp rX, #N; ldrlo pc, [pc, rX, lsl #2]; b default`
        followed by N absolute addresses. Returns False if insn isn't that."""
        mem = insn.operands[1]
        if (len(insn.operands) != 2 or mem.type != ARM_OP_MEM or _reg_num(insn, mem.mem.base) != PC
                or not mem.mem.index or mem.subtracted or mem.shift.value != 2 or insn.writeback):
            return False
        index = _reg_num(insn, mem.mem.index)
        bound = None
        cmp = self._bound_check(fn, insn, index)
        if cmp is not None:
            bound = {ARM_CC_LO: cmp, ARM_CC_LS: cmp + 1}.get(insn.cc)
        if insn.cc != ARM_CC_AL:
            work.append((nxt, False))
        if bound is None:
            self.unresolved.add(insn.address)
            return True
        table = insn.address + 8
        raw = self.program.read(table, 4 * bound)
        if raw is None:
            self.unresolved.add(insn.address)
            return True
        targets = [int.from_bytes(raw[4 * i:4 * i + 4], 'little') for i in range(bound)]
        fn.data.update((table + 4 * i, 4) for i in range(bound))
        self.jump_tables[insn.address] = targets
        for t in targets:
            if t & 1 or not self.in_text(t):
                raise Rejected(f'{insn.address:08x}: switch table entry {t:08x} is not ARM code')
            if fn.addr <= t:
                work.append((t, False))
            else:
                self._call(fn, t, False, insn)
        return True

    def _offset_table(self, fn, insn, index):
        """The entries of the table an `add pc, pc, rI, lsl #2` index was
        loaded from, or None if it wasn't loaded from one we can read.

        gcc -O2 compiles a dense switch to
            cmp   rK, #N          ; the bound (bhi to the default)
            add   rO, rK, rK      ; or lsl: scale the case number
            ldrsh rI, [rT, rO]    ; an offset from the table at rT
            add   pc, pc, rI, lsl #2
        with rT loaded from the literal pool, maybe much earlier. We follow
        that back along the code before the jump."""
        load = next((p for p in self._back(fn, insn.address, 12) if self._writes(p, index)), None)
        if load is None or load.id not in (ARM_INS_LDRB, ARM_INS_LDRSB, ARM_INS_LDRH, ARM_INS_LDRSH):
            return None
        mem = load.operands[1]
        if mem.type != ARM_OP_MEM or not mem.mem.index or mem.subtracted or len(load.operands) != 2:
            return None
        size = 1 if load.id in (ARM_INS_LDRB, ARM_INS_LDRSB) else 2
        base_reg, reg = _reg_num(load, mem.mem.base), _reg_num(load, mem.mem.index)
        offset_reg = reg
        shift = mem.shift.value if mem.shift.type else 0
        # The address is base + offset * (1 << shift). gcc sometimes adds the
        # offset into the base first (`add r2, r2, lr; ldrh r2, [r2, lr]`),
        # which makes each entry `extra` offsets further apart.
        extra = 0

        # The case number: follow scalings of the offset back to a cmp bound.
        bound, at, scale = None, load.address, 1
        while bound is None:
            p = next((p for p in self._back(fn, at, 16)
                      if self._writes(p, reg) or (p.id == ARM_INS_CMP and _reg_num(p, p.operands[0].reg) == reg)),
                     None)
            if p is None:
                return None
            ops = p.operands
            if p.id == ARM_INS_CMP:
                if ops[1].type != ARM_OP_IMM:
                    return None
                bound = ops[1].imm + 1
            elif p.id == ARM_INS_ADD and len(ops) == 3 and ops[1].type == ops[2].type == ARM_OP_REG                     and ops[1].reg == ops[2].reg and not ops[2].shift.type:
                reg, at = _reg_num(p, ops[1].reg), p.address      # rO = rK + rK
                scale *= 2
            elif p.id == ARM_INS_LSL and len(ops) == 3 and ops[2].type == ARM_OP_IMM:
                reg, at = _reg_num(p, ops[1].reg), p.address
                scale <<= ops[2].imm
            else:
                return None         # the index changes in some other way before any bound check

        # The table's address: the last write to rT before the load.
        at = load.address
        while True:
            p = next((p for p in self._back(fn, at, 400) if self._writes(p, base_reg)), None)
            if p is None:
                return None
            ops = p.operands
            if p.id == ARM_INS_ADD and len(ops) == 3 and ops[1].type == ops[2].type == ARM_OP_REG                     and not ops[2].shift.type and offset_reg in (_reg_num(p, ops[1].reg), _reg_num(p, ops[2].reg)):
                regs = [_reg_num(p, ops[1].reg), _reg_num(p, ops[2].reg)]
                regs.remove(offset_reg)
                base_reg, at, extra = regs[0], p.address, extra + 1
                continue
            break
        if p.id != ARM_INS_LDR or p.operands[1].type != ARM_OP_MEM or                 _reg_num(p, p.operands[1].mem.base) != PC:
            return None
        raw = self.program.read(_address(p).const, 4)
        if raw is None:
            return None
        base = int.from_bytes(raw, 'little')

        stride = scale * ((1 << shift) + extra)
        raw = self.program.read(base, (bound - 1) * stride + size)
        if raw is None:
            return None
        signed = load.id in (ARM_INS_LDRSB, ARM_INS_LDRSH)
        return [int.from_bytes(raw[i * stride:i * stride + size], 'little', signed=signed) for i in range(bound)]

    @staticmethod
    def _back(fn, addr, limit):
        """Up to `limit` instructions of fn before addr, nearest first, in
        address order (straight-line code; literal pools are skipped)."""
        step = 2 if fn.thumb else 4
        a, n = addr - step, 0
        while a >= fn.addr and n < limit:
            p = fn.insns.get(a)
            if p is not None:
                yield p
                n += 1
            a -= step

    @staticmethod
    def _writes(insn, reg):
        """Whether insn writes core register `reg` as its first operand."""
        if insn.id in (ARM_INS_CMP,) or not insn.operands or insn.operands[0].type != ARM_OP_REG:
            return False
        if insn.mnemonic.startswith(('str', 'stm', 'push', 'cmn', 'tst', 'teq', 'vst', 'b')):
            return False
        return _reg_num(insn, insn.operands[0].reg) == reg

    # ------------------------------------------------------------ the whole program

    def closure(self, roots, known, data_words, own_data=None):
        """Explore `roots` and every function they call. `known` maps the
        function starts already explored (addr -> Explored); it is extended.
        Returns the newly explored functions."""
        new = {}
        # A stack: the last root's whole call tree is explored before the
        # next root is looked at, so put the roots you trust most last.
        work = [(addr, thumb, None, (addr, thumb)) for addr, thumb in roots]
        while True:
            if not work:
                # A start registered in an earlier round whose caller no
                # longer calls it (that caller's extent changed) still cuts
                # its neighbour short; so explore it too, as the function the
                # translator will see falling into it.
                work = [(a, *self._start_info[a], None) for a in self.starts
                        if a not in new and a not in known and a in self._start_info]
                work = [(a, thumb, None, root) for a, thumb, root, _ in work]
                if not work:
                    break
            addr, thumb, caller, root = work.pop()
            if addr in known or addr in new:
                have = known.get(addr) or new.get(addr)
                if have.thumb != thumb:
                    mode = 'Thumb' if thumb else 'ARM'
                    where = f'from {caller:08x}' if caller is not None else 'as a start'
                    e = Rejected(f'{addr:08x} is called as {mode} {where}, '
                                 f'but as {"Thumb" if have.thumb else "ARM"} elsewhere')
                    e.root = root
                    raise e
                continue
            if addr not in self.starts_set:
                self._add_start(addr)
            self._start_info[addr] = (thumb, root)
            try:
                own = own_data.get(root, frozenset()) if own_data else frozenset()
                fn = self.explore(addr, thumb, self.starts, data_words, own)
            except Rejected as e:
                e.root = root
                raise
            if fn.root != root:
                fn = replace(fn, root=root)
            new[addr] = fn
            work.extend((t, th, a, root) for t, th, a in fn.call_sites)
        return new

    def _add_start(self, addr):
        i = bisect_right(self.starts, addr)
        self.starts.insert(i, addr)
        self.starts_set.add(addr)
        if self._added is not None:
            self._added.append(addr)

    def run(self, extra=()):
        """Returns (functions, report)."""
        entry = self.program.entry
        roots = ({(entry & ~1, bool(entry & 1))} if entry is not None else set()) | set(extra)
        accepted, rejected = set(), {}
        while True:
            try:
                fns = self._fixpoint(roots, accepted)
            except Rejected as e:
                if e.root not in accepted:
                    raise LoadError(f'code reached from the entry point: {e}') from None
                # Fine on its own, but not together with the others: drop it.
                accepted.discard(e.root)
                rejected[e.root[0] | e.root[1]] = f'with the other functions: {e}'
                continue
            # Real code that runs straight into a guessed start means the
            # guess is the middle of a function (a data word that only looks
            # like an address, e.g. in a font bitmap). Splitting there would
            # break that function's returns.
            # A guess starting inside some function's literal pool is data.
            # Drop those first: decoding a pool, they would "run into" the
            # real function after it.
            pools = self._data_words(fns)
            in_pool = {g for g in accepted if g[0] in pools}
            if in_pool:
                for g in in_pool:
                    accepted.discard(g)
                    rejected[g[0] | g[1]] = 'inside the literal data of another function'
                continue
            falls = {a for fn in fns.values() for a in fn.falls_into}
            middle = {g for g in accepted if g[0] in falls}
            if middle:
                for g in middle:
                    accepted.discard(g)
                    rejected[g[0] | g[1]] = 'the middle of a function: code before it runs straight into it'
                continue
            # A guess must also be code we can translate: data that happens
            # to decode (`ldrdeq r0, r1, [r0], -lr`) usually can't be.
            bad = self._untranslatable(fns, roots)
            if bad:
                for root, why in bad.items():
                    accepted.discard(root)
                    rejected[root[0] | root[1]] = f'not translatable: {why}'
                continue
            candidates = self._candidates(fns)
            data_words = self._data_words(fns)
            code = {a: insn for fn in fns.values() for a, insn in fn.insns.items()}
            added = False
            known = dict(fns)       # grows with each accepted candidate, so later ones must agree with it
            for value in sorted(candidates):
                addr, thumb = value & ~1, bool(value & 1)
                if addr in known or value in rejected or (addr, thumb) in accepted:
                    continue
                if not thumb and addr % 4:
                    continue
                if addr in code:
                    # Inside a function we already know: splitting it there
                    # would break its returns. A real function pointer to
                    # such an address stops loudly at runtime instead, naming
                    # it (add it with --entries).
                    continue
                found = self._try_guess((addr, thumb), known, data_words, code)
                if isinstance(found, str):
                    rejected[value] = found
                    continue
                known.update(found)
                accepted.add((addr, thumb))
                added = True
            if not added:
                break
        return fns, {'rejected': rejected, 'pointer_functions': len(accepted),
                     'unresolved_jump_tables': ' '.join(f'{a:08x}' for a in sorted(self.unresolved))}

    def _try_guess(self, root, known, data_words, code):
        """Explore a guessed function start and everything it calls. Returns
        the new functions, or the reason it isn't code. Its own literal data
        stops its own fall-throughs (a call that never returns, then a pool),
        so it is explored again until that settles."""
        own = set()
        for _ in range(8):
            self._added = []
            try:
                found = self.closure([root], known, data_words, {root: own})
                for fn in found.values():
                    bad = next((a for a, size in fn.data if any(a + i in code for i in range(size))), None)
                    if bad is not None:
                        raise Rejected(f'{fn.addr:08x} loads literal data from {bad:08x}, which is code')
                words = self._data_words(found)
                if words <= own:
                    return found
                own |= words
                failure = None
            except Rejected as e:
                failure = str(e)
            finally:
                for a in self._added:               # undo the starts this attempt added
                    self.starts.remove(a)
                    self.starts_set.discard(a)
                self._added = None
            if failure is not None:
                return failure
        return 'its literal data never settled'

    def _untranslatable(self, fns, roots):
        """{guess root: reason} for guessed functions the translator refuses."""
        entries = {a: Function(f'sub_{a:08x}', a, b'', f.thumb) for a, f in fns.items()}
        bad = {}
        for fn in fns.values():
            if fn.root in roots or fn.root in bad:
                continue
            shape = (fn.addr, fn.thumb, len(fn.insns), max(fn.insns), frozenset(fn.calls), frozenset(fn.falls_into))
            if shape not in self._translatable:
                func = _function(self.program, fn)
                insns = [fn.insns[a] for a in sorted(fn.insns)]
                try:
                    translate_function(func, insns, entries, self.program)
                    self._translatable[shape] = None
                except (UnsupportedInstruction, ValueError) as e:
                    self._translatable[shape] = str(e)
            if self._translatable[shape] is not None:
                bad[fn.root] = self._translatable[shape]
        return bad

    def _fixpoint(self, roots, guesses=()):
        """Explore from `roots` (trusted) and `guesses` (functions found
        through pointers) until function starts and literal data stop
        changing. Both only ever grow, so this ends. Raises Rejected, with
        the root that reached bad code."""
        self.starts, self.starts_set = [], set()
        for addr, _ in list(roots) + list(guesses):
            self._add_start(addr)
        data_words, own = set(), {}
        order = sorted(guesses) + sorted(roots)         # trusted roots are explored first
        while True:
            before = (len(self.starts), len(data_words), sum(map(len, own.values())))
            fns = self.closure(order, {}, data_words, own)
            # Only trusted code decides what is data for everyone: a wrong
            # guess must not turn real code into "literal pool". A guess's
            # data only counts for the functions it found itself.
            for f in fns.values():
                words = data_words if f.root in roots else own.setdefault(f.root, set())
                for a, size in f.data:
                    words.update(range(a, a + size))
            if (len(self.starts), len(data_words), sum(map(len, own.values()))) == before:
                return fns

    @staticmethod
    def _data_words(fns):
        words = set()
        for fn in fns.values():
            for addr, size in fn.data:
                words.update(range(addr, addr + size))
        return words

    def _candidates(self, fns):
        """Words that point into the code: in literal pools, and in every
        aligned word of the read-only and data segments."""
        out = set()
        for fn in fns.values():
            out |= fn.pointers
        out |= getattr(self.program, 'pointer_hints', set())
        exact = getattr(self.program, 'pointer_candidates', None)
        if exact is not None:
            # A CRO's relocations say exactly which words point into its code.
            return out | exact
        for seg in self.program.segments:
            if seg is self.text:
                continue
            data = seg.data
            run = []
            for off in range(0, len(data) - 3, 4):
                value = int.from_bytes(data[off:off + 4], 'little')
                if self.in_text(value & ~1):
                    out.add(value)
                # ARMCC keeps its table of C++ static constructors as offsets
                # from each entry's own address (position-independent). One
                # such word is likely chance; a run of them is that table.
                target = (seg.vaddr + off + value) & 0xFFFFFFFF
                if self.in_text(target & ~1):
                    run.append(target)
                else:
                    if len(run) >= 4:
                        out.update(run)
                    run = []
            if len(run) >= 4:
                out.update(run)
        return out


def to_functions(program, fns):
    """elf_loader.Function objects (code_ranges = runs of reached instructions)
    and the literal-pool data ranges, for the translator."""
    functions = []
    data = set()
    for addr in sorted(fns):
        functions.append(_function(program, fns[addr]))
        for a, size in fns[addr].data:
            data.add((a, a + size))
    return functions, sorted(data)


def _function(program, ex):
    """An elf_loader.Function whose code ranges are the runs of reached instructions."""
    ranges, start, end = [], None, None
    for a in sorted(ex.insns):
        size = ex.insns[a].size
        if start is None:
            start, end = a, a + size
        elif a == end:
            end += size
        else:
            ranges.append((start, end))
            start, end = a, a + size
    ranges.append((start, end))
    code = program.read(ex.addr, ranges[-1][1] - ex.addr)
    return Function(f'sub_{ex.addr:08x}', ex.addr, code, ex.thumb, ranges)


def discover(program, extra=()):
    """Fill in a symbol-less Program's functions and literal data.
    Returns (functions, report)."""
    d = Discovery(program)
    fns, report = d.run(extra)
    functions, data_ranges = to_functions(program, fns)
    program.functions = functions
    program.data_ranges = data_ranges
    program.jump_tables = d.jump_tables
    report['functions'] = len(functions)
    report['instructions'] = sum(len(f.insns) for f in fns.values())
    return functions, report
