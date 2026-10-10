"""Capstone wrapper for ARM and Thumb (ARMv6K: Thumb-1 only)."""

from capstone import CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB, Cs
from capstone.arm import ARM_INS_BL, ARM_INS_BLX


class DisasmError(Exception):
    pass


_ENGINES = {}


def _engine(thumb):
    if thumb not in _ENGINES:
        md = Cs(CS_ARCH_ARM, CS_MODE_THUMB if thumb else CS_MODE_ARM)
        md.detail = True    # operands, condition code, shifts
        _ENGINES[thumb] = md
    return _ENGINES[thumb]


def disassemble(code, addr, thumb=False):
    """Decode every instruction in `code`, which starts at guest address `addr`."""
    insns = list(_engine(thumb).disasm(code, addr))

    # Capstone silently stops at the first word it can't decode.
    decoded = sum(insn.size for insn in insns)
    if decoded != len(code):
        size = 2 if thumb else 4
        word = int.from_bytes(code[decoded:decoded + size], 'little')
        raise DisasmError(f'{addr + decoded:08x}: cannot decode instruction {word:0{2 * size}x}')

    if thumb:
        # Capstone also decodes Thumb-2, which the 3DS's ARMv6K CPU doesn't
        # have. The only 32-bit Thumb instructions on ARMv6K are bl and blx.
        for insn in insns:
            if insn.size == 4 and insn.id not in (ARM_INS_BL, ARM_INS_BLX):
                raise DisasmError(f'{insn.address:08x}: `{insn.mnemonic} {insn.op_str}` is a 32-bit '
                                  'Thumb-2 instruction, which ARMv6K (the 3DS CPU) does not have')
    return insns


def disassemble_arm(code, addr):
    return disassemble(code, addr, thumb=False)
