"""CLI: python -m recomp <input.elf> -o out/gen.c"""

import argparse
import sys
from pathlib import Path

from .disasm import DisasmError, disassemble_arm
from .elf_loader import LoadError, load_functions
from .emit_c import c_name, emit
from .translate import UnsupportedInstruction, translate_function


def main(argv=None):
    parser = argparse.ArgumentParser(prog='python -m recomp',
                                     description='Recompile ARM code in an ELF file to C.')
    parser.add_argument('input', help='ARM ELF file (.o or linked .elf)')
    parser.add_argument('-o', '--output', required=True, type=Path,
                        help='C file to write; a matching .h is written next to it')
    args = parser.parse_args(argv)

    try:
        functions = load_functions(args.input)
        if not functions:
            raise LoadError(f'{args.input}: no function symbols found')

        translated = []
        for fn in functions:
            if fn.thumb:
                raise LoadError(f'{fn.addr:08x} {fn.name}: Thumb code is not supported yet (M4)')
            insns = disassemble_arm(fn.code, fn.addr)
            translated.append((fn, translate_function(fn, insns)))

        header = args.output.with_suffix('.h')
        c_src, h_src = emit(translated, header.name)
    except (LoadError, DisasmError, UnsupportedInstruction) as e:
        # Nothing has been written yet, so a failed run leaves no stale output.
        print(f'recomp: error: {e}', file=sys.stderr)
        return 1

    args.output.parent.mkdir(parents=True, exist_ok=True)
    header.write_text(h_src, newline='\n')
    args.output.write_text(c_src, newline='\n')

    for fn, body in translated:
        print(f'  {c_name(fn.addr)}  {fn.name}  ({len(body)} instructions)')
    print(f'wrote {args.output} and {header.name}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
