"""Capstone wrapper. ARM mode only for now; Thumb arrives in M4."""

from capstone import CS_ARCH_ARM, CS_MODE_ARM, Cs


class DisasmError(Exception):
    pass


def disassemble_arm(code, addr):
    """Decode every instruction in `code`, which starts at guest address `addr`."""
    md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    md.detail = True    # operands, condition code, S flag, shifts
    insns = list(md.disasm(code, addr))

    # Capstone silently stops at the first word it can't decode.
    decoded = sum(insn.size for insn in insns)
    if decoded != len(code):
        word = int.from_bytes(code[decoded:decoded + 4], 'little')
        raise DisasmError(f'{addr + decoded:08x}: cannot decode instruction word {word:08x}')
    return insns
