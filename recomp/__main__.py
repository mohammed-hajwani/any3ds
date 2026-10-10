"""CLI: python -m recomp <input.elf> -o out/gen.c"""

import argparse
import sys
from pathlib import Path

from .disasm import DisasmError, disassemble_arm
from .elf_loader import LoadError, load_program
from .emit_c import c_name, emit
from .translate import UnsupportedInstruction, translate_function


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

        entries = {fn.addr: fn for fn in program.functions}
        translated = []
        for fn in program.functions:
            if fn.thumb:
                raise LoadError(f'{fn.addr:08x} {fn.name}: Thumb code is not supported yet (M4)')
            if not fn.code_ranges:
                raise LoadError(f'{fn.addr:08x} {fn.name}: contains no ARM code')
            # Only decode code; literal pools inside the function are data.
            insns = []
            for start, end in fn.code_ranges:
                insns += disassemble_arm(fn.code[start - fn.addr:end - fn.addr], start)
            translated.append(translate_function(fn, insns, entries, program))

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
