"""CLI: python -m recomp <input.elf> -o out/gen.c"""

import argparse
import re
import sys
from pathlib import Path

from capstone.arm import ARM_INS_B, ARM_INS_BLX

from . import cro as cro_format
from . import emit_c, translate
from .ctr_loader import load_ctr
from .disasm import DisasmError, disassemble
from .discover import discover
from .elf_loader import Function, LoadError, load_program
from .emit_c import c_name, emit
from .translate import TranslatedFunction, UnsupportedInstruction, direct_branch_target, translate_function


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
        where = f'{insn.address:08x}: {insn.mnemonic} target' if insn else 'entry point'
        raise LoadError(f'{where} {target:08x} is not inside any known function or code region')
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
    if program.entry is not None and (program.entry & ~1) not in entries:
        # The program's entry point (e.g. libctru's crt0 `_start`) is often a
        # plain label, not a function symbol.
        fn = _orphan_function(program, program.entry & ~1, None)
        fn.name = '_entry'
        entries[fn.addr] = fn
        functions.append(fn)
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


def _load_without_symbols(args):
    """A Program with no symbols: a code.bin (directory, .cxi or .3ds), or
    an ELF whose symbols we ignore. Functions come from discovery."""
    path = Path(args.input)
    if path.is_dir() or path.suffix.lower() in ('.cxi', '.3ds', '.cci', '.app'):
        program = load_ctr(path)
    else:
        program = load_program(path)
        program.functions, program.data_ranges, program.code_regions = [], [], []
        program.data_symbols = {}
    extra = []
    if args.entries:
        for line in Path(args.entries).read_text().splitlines():
            line = line.split('#')[0].strip()
            if line:
                value = int(line, 16)
                extra.append((value & ~1, bool(value & 1)))
    functions, report = discover(program, extra)
    if path.suffix.lower() in ('.cxi', '.3ds', '.cci'):
        # Words the CRO loader patches in the main program (static.crs:
        # addresses of functions in modules) must be read at run time.
        crs = _static_crs(path)
        if crs is not None:
            patched = {crs.segments[p.at[0]].offset + p.at[1] for imp in crs.imports for p in imp.patches}
            program.data_ranges = cro_format.without_patched(program.data_ranges, patched)
    decoded = {fn.addr: _decode(fn) for fn in functions}
    lines = [f'{k}: {v}' for k, v in report.items() if k != 'rejected']
    lines += [f'rejected {value:08x}: {why}' for value, why in sorted(report['rejected'].items())]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.with_suffix('.discovery.txt').write_text('\n'.join(lines) + '\n', newline='\n')
    print(f'discovery: {report["functions"]} functions, {report["instructions"]} instructions, '
          f'{report["pointer_functions"]} found through pointers, {len(report["rejected"])} pointers rejected')
    return program, functions, decoded


def _static_crs(path):
    """The game's static.crs (what the main program exports to its CRO
    modules and imports from them), or None if it has no RomFS or no CRS."""
    from .romfs import RomFS
    try:
        fs = RomFS(path)
    except LoadError:
        return None
    for name in ('static.crs', 'cro/static.crs'):
        try:
            return cro_format.parse(fs.read(name))
        except LoadError:
            continue
    return None


def _load_cro(args):
    """A CRO module, recompiled to code relative to its load address."""
    from .romfs import RomFS
    path = Path(args.input)
    data = path.read_bytes() if path.suffix.lower() == '.cro' else RomFS(path).read(args.cro)
    module = cro_format.parse(data)
    program, roots = cro_format.relocated_program(module)
    functions, report = discover(program, roots)
    program.data_ranges = cro_format.without_patched(program.data_ranges, program.patched)
    decoded = {fn.addr: _decode(fn) for fn in functions}
    print(f'module {module.name}: {report["functions"]} functions, {report["instructions"]} instructions, '
          f'{len(roots)} exported or hooks, {report["pointer_functions"]} found through relocations')
    return module, program, functions, decoded


def _report_failures(path, failures):
    """--keep-going: every untranslated function, grouped by the reason."""
    by_reason = {}
    for fn, e in failures:
        text = str(e).split(': ', 1)[1]
        key = re.sub(r'(0x)?[0-9a-f]{8}', 'X', text)
        by_reason.setdefault(key, []).append((fn, e))
    lines = [f'{len(failures)} functions not translated', '']
    for key, items in sorted(by_reason.items(), key=lambda kv: -len(kv[1])):
        lines.append(f'{len(items):6}  {key}')
        for fn, e in items[:3]:
            lines.append(f'            e.g. {e}')
    path.write_text('\n'.join(lines) + '\n', newline='\n')
    print(f'{len(failures)} functions not translated (--keep-going): see {path}')


def main(argv=None):
    parser = argparse.ArgumentParser(prog='python -m recomp',
                                     description='Recompile ARM code in an ELF file to C.')
    parser.add_argument('input', help='ARM ELF file (.o or linked .elf), or a 3DS program: a directory with '
                                      'code.bin + exheader.bin, or a decrypted .cxi/.3ds')
    parser.add_argument('-o', '--output', required=True, type=Path,
                        help='C file to write; a matching .h is written next to it')
    parser.add_argument('--trace', action='store_true',
                        help='call RECOMP_TRACE before every instruction (for differential testing)')
    parser.add_argument('--no-symbols', action='store_true',
                        help='ignore an ELF\'s symbols and find the functions by following the code '
                             '(always the case for a code.bin)')
    parser.add_argument('--keep-going', action='store_true',
                        help='a function that can\'t be translated becomes a stub that stops loudly when '
                             'called, instead of failing the run; every failure is listed in OUTPUT.failures.txt')
    parser.add_argument('--split', type=int, metavar='N',
                        help='put the functions in extra files of N functions each (OUTPUT_001.c, ...) '
                             'so a big program compiles in parallel')
    parser.add_argument('--cro', metavar='MODULE',
                        help='recompile the CRO module MODULE (a path in the RomFS of INPUT, a decrypted dump; '
                             'or give a .cro file as INPUT) into code relative to its load address')
    parser.add_argument('--entries', metavar='FILE',
                        help='extra function starts, one hex address per line (bit 0 = Thumb)')
    args = parser.parse_args(argv)

    try:
        path = Path(args.input)
        module = None
        if args.cro or path.suffix.lower() == '.cro':
            module, program, functions, decoded = _load_cro(args)
            translate.RELOCATABLE = True
            emit_c.NAME_PREFIX = emit_c.module_prefix(module.name)
        elif args.no_symbols or path.is_dir() or path.suffix.lower() in ('.cxi', '.3ds', '.cci', '.app'):
            program, functions, decoded = _load_without_symbols(args)
        else:
            program = load_program(args.input)
            if not program.functions:
                raise LoadError(f'{args.input}: no function symbols found')
            functions, decoded = discover_functions(program)
        entries = {fn.addr: fn for fn in functions}
        translated, failures = [], []
        for fn in functions:
            try:
                translated.append(translate_function(fn, decoded[fn.addr], entries, program))
            except UnsupportedInstruction as e:
                if not args.keep_going:
                    raise
                failures.append((fn, e))
                why = str(e).replace('\\', '\\\\').replace('"', '\\"')
                translated.append(TranslatedFunction(fn, [(fn.addr, f'not translated: {e}', [
                    f'recomp_untranslated(ctx, {fn.addr:#010x}u, "{why}");'])], set(), False))
        if failures:
            _report_failures(args.output.with_suffix('.failures.txt'), failures)

        header = args.output.with_suffix('.h')
        if module is not None:
            c_src, h_src = emit_c.emit_module(translated, header.name, module.name, split=args.split)
        else:
            c_src, h_src = emit(translated, header.name, program, trace=args.trace, split=args.split)
    except (LoadError, DisasmError, UnsupportedInstruction) as e:
        # Nothing has been written yet, so a failed run leaves no stale output.
        print(f'recomp: error: {e}', file=sys.stderr)
        return 1

    args.output.parent.mkdir(parents=True, exist_ok=True)
    header.write_text(h_src, newline='\n')
    if args.split:
        args.output.write_text(c_src[0], newline='\n')
        for i, chunk in enumerate(c_src[1:], 1):
            args.output.with_name(f'{args.output.stem}_{i:03d}.c').write_text(chunk, newline='\n')
    else:
        args.output.write_text(c_src, newline='\n')
    if module is not None:
        # For the table of modules a program links (tools/build_dump.sh).
        args.output.with_suffix('.module').write_text(re.sub(r'\W', '_', module.name) + '\n', newline='\n')

    for tf in translated:
        n = sum(1 for _, _, lines in tf.body if lines)
        print(f'  {emit_c.c_name(tf.fn.addr)}  {tf.fn.name}  ({n} instructions)')
    extra = f' (+ {len(c_src) - 1} files of functions)' if args.split else ''
    print(f'wrote {args.output}{extra} and {header.name}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
