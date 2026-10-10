"""CLI: python -m recomp <input.elf> -o out/gen.c"""

import argparse
import sys
from pathlib import Path

from capstone.arm import ARM_INS_B, ARM_INS_BLX

from .disasm import DisasmError, disassemble
from .elf_loader import Function, LoadError, load_program
from .emit_c import c_name, emit
from .translate import UnsupportedInstruction, direct_branch_target, translate_function


def _decode(fn):
    """Disassemble a function's code ranges (literal pools inside are data)."""
    if not fn.code_ranges:
        raise LoadError(f'{fn.addr:08x} {fn.name}: contains no code')
    insns = []
    for start, end in fn.code_ranges:
        insns += disassemble(fn.code[start - fn.addr:end - fn.addr], start, fn.thumb)
    return insns


def _orphan_function(program, target, insn):
    """Code with no function symbol at all, e.g. the helper newlib's
    hand-written strcmp keeps just before its own symbol. Accepted only
    where the assembler marked code ($a/$t); it runs to the end of that
    region or the next function, whichever comes first."""
    region = program.code_region(target)
    if region is None:
        raise LoadError(f'{insn.address:08x}: {insn.mnemonic} target {target:08x} '
                        'is not inside any known function or code region')
    _, end, thumb = region
    end = min([end] + [f.addr for f in program.functions if f.addr > target])
    code = program.read(target, end - target)
    if code is None:
        raise LoadError(f'{target:08x}: code region outside the program image')
    return Function(f'code_{target:08x}', target, code, thumb, [(target, end)])


def discover_functions(program):
    """Symbol table functions, plus every call/branch target that isn't one.

    Hand-written code (libgcc's division routines, for instance) calls and
    tail-branches into the *middle* of other functions: alternate entry
    points with no symbol. Each such target becomes a function of its own,
    running from the target to the end of the function containing it; that
    code is simply recompiled twice. Returns (functions, decoded insns).
    """
    functions = list(program.functions)
    entries = {fn.addr: fn for fn in functions}
    decoded = {}
    queue = list(functions)
    while queue:
        fn = queue.pop()
        decoded[fn.addr] = insns = _decode(fn)
        for insn in insns:
            target = direct_branch_target(insn)
            if target is None:
                continue
            if insn.id == ARM_INS_BLX and fn.thumb:
                target &= ~3                # Thumb blx #imm goes to ARM code: word-aligned
            if target in entries:
                continue
            if insn.id == ARM_INS_B and fn.addr <= target < fn.end:
                continue                    # an ordinary branch within the function
            container = next((f for f in program.functions if f.addr < target < f.end), None)
            if container is not None:
                off = target - container.addr
                new = Function(f'{container.name}+0x{off:x}', target, container.code[off:], container.thumb,
                               [(max(s, target), e) for s, e in container.code_ranges if e > target])
            else:
                new = _orphan_function(program, target, insn)
            entries[target] = new
            functions.append(new)
            queue.append(new)
    functions.sort(key=lambda f: f.addr)
    return functions, decoded


def main(argv=None):
    parser = argparse.ArgumentParser(prog='python -m recomp',
                                     description='Recompile ARM code in an ELF file to C.')
    parser.add_argument('input', help='ARM ELF file (.o or linked .elf)')
    parser.add_argument('-o', '--output', required=True, type=Path,
                        help='C file to write; a matching .h is written next to it')
    parser.add_argument('--trace', action='store_true',
                        help='call RECOMP_TRACE before every instruction (for differential testing)')
    args = parser.parse_args(argv)

    try:
        program = load_program(args.input)
        if not program.functions:
            raise LoadError(f'{args.input}: no function symbols found')

        functions, decoded = discover_functions(program)
        entries = {fn.addr: fn for fn in functions}
        translated = [translate_function(fn, decoded[fn.addr], entries, program) for fn in functions]

        header = args.output.with_suffix('.h')
        c_src, h_src = emit(translated, header.name, program, trace=args.trace)
    except (LoadError, DisasmError, UnsupportedInstruction) as e:
        # Nothing has been written yet, so a failed run leaves no stale output.
        print(f'recomp: error: {e}', file=sys.stderr)
        return 1

    args.output.parent.mkdir(parents=True, exist_ok=True)
    header.write_text(h_src, newline='\n')
    args.output.write_text(c_src, newline='\n')

    for tf in translated:
        n = sum(1 for _, _, lines in tf.body if lines)
        print(f'  {c_name(tf.fn.addr)}  {tf.fn.name}  ({n} instructions)')
    print(f'wrote {args.output} and {header.name}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
