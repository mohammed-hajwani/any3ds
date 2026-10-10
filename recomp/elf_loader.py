"""Read an ARM ELF: functions (and which of their bytes are code), the
loadable segments, and data symbols."""

from bisect import bisect_right
from dataclasses import dataclass, field

from elftools.elf.elffile import ELFFile
from elftools.elf.sections import SymbolTableSection


class LoadError(Exception):
    pass


@dataclass
class Function:
    name: str
    addr: int               # guest address (bit 0 cleared)
    code: bytes             # every byte of the symbol, code and data
    thumb: bool             # symbol value had bit 0 set => Thumb code
    code_ranges: list = field(default_factory=list)   # [(start, end)] of ARM code

    @property
    def end(self):
        return self.addr + len(self.code)


@dataclass
class Segment:
    vaddr: int
    data: bytes             # file contents (p_filesz bytes)
    memsz: int              # the rest up to memsz is zero (.bss)
    writable: bool


@dataclass
class Program:
    functions: list
    segments: list
    data_symbols: dict      # name -> guest address (linked executables only)
    data_ranges: list       # sorted [(start, end)] marked $d by the assembler

    def literal(self, addr, size):
        """Bytes of a read-only literal at `addr`, or None if it isn't one.

        Only data the assembler marked `$d` inside a non-writable segment
        qualifies: nothing can change it at runtime, so it can be folded
        into the generated code as a constant.
        """
        i = bisect_right(self.data_ranges, (addr, float('inf'))) - 1
        if i < 0 or not (self.data_ranges[i][0] <= addr and addr + size <= self.data_ranges[i][1]):
            return None
        for seg in self.segments:
            if seg.vaddr <= addr and addr + size <= seg.vaddr + len(seg.data):
                if seg.writable:
                    return None
                return seg.data[addr - seg.vaddr:addr - seg.vaddr + size]
        return None


def _mapping_symbols(symtab):
    """ARM mapping symbols per section: {shndx: sorted [(addr, 'a'|'d'|'t')]}.

    The assembler marks where ARM code ($a), Thumb code ($t) and data ($d,
    e.g. literal pools) start, so we never decode data as instructions.
    """
    marks = {}
    for sym in symtab.iter_symbols():
        name = sym.name
        if len(name) >= 2 and name[0] == '$' and name[1] in 'adt' and (len(name) == 2 or name[2] == '.'):
            if isinstance(sym['st_shndx'], int):
                marks.setdefault(sym['st_shndx'], []).append((sym['st_value'], name[1]))
    for lst in marks.values():
        lst.sort()
    return marks


def _code_ranges(fn, marks):
    """Split a function's byte range into ARM code ranges using mapping symbols."""
    state = 'a'                             # no mapping symbol at all: assume ARM
    for addr, kind in marks:
        if addr <= fn.addr:
            state = kind
    boundaries = [(a, k) for a, k in marks if fn.addr < a < fn.end]

    ranges, start = [], fn.addr
    for addr, kind in boundaries + [(fn.end, None)]:
        if state == 't' and addr > start:
            raise LoadError(f'{fn.addr:08x} {fn.name}: Thumb code is not supported yet (M4)')
        if state == 'a' and addr > start:
            ranges.append((start, addr))
        start, state = addr, kind
    return ranges


def load_program(path):
    with open(path, 'rb') as f:
        elf = ELFFile(f)
        if elf['e_machine'] != 'EM_ARM' or elf.elfclass != 32 or not elf.little_endian:
            raise LoadError(f'{path}: not a 32-bit little-endian ARM ELF')

        symtab = elf.get_section_by_name('.symtab')
        if not isinstance(symtab, SymbolTableSection):
            raise LoadError(f'{path}: no symbol table')

        relocatable = elf['e_type'] == 'ET_REL'
        if relocatable:
            # In an unlinked .o, branch/call targets are placeholders that the
            # linker patches using these relocations. We don't apply them.
            for section in elf.iter_sections():
                if section['sh_type'] in ('SHT_REL', 'SHT_RELA') and section.num_relocations():
                    raise LoadError(f'{path}: unlinked object has relocations '
                                    f'({section.name}); link it first')

        marks = _mapping_symbols(symtab)
        functions, data_symbols = [], {}
        for sym in symtab.iter_symbols():
            shndx = sym['st_shndx']
            if not isinstance(shndx, int) or sym['st_size'] == 0:   # SHN_UNDEF, SHN_ABS, ...
                continue
            kind = sym['st_info']['type']
            if kind == 'STT_OBJECT' and not relocatable:
                data_symbols[sym.name] = sym['st_value']
            if kind != 'STT_FUNC':
                continue

            section = elf.get_section(shndx)
            value = sym['st_value']
            # Bit 0 of a function symbol is the Thumb bit, not part of the address.
            addr = value & ~1
            # In a relocatable .o, st_value is an offset into its section;
            # in a linked executable it is a virtual address.
            offset = addr if relocatable else addr - section['sh_addr']
            code = section.data()[offset:offset + sym['st_size']]
            fn = Function(sym.name, addr, code, bool(value & 1))
            if not fn.thumb:
                fn.code_ranges = _code_ranges(fn, marks.get(shndx, []))
            functions.append(fn)

        data_ranges = []
        for shndx, lst in marks.items():
            section = elf.get_section(shndx)
            sec_end = (0 if relocatable else section['sh_addr']) + section['sh_size']
            for i, (addr, kind) in enumerate(lst):
                if kind == 'd':
                    end = lst[i + 1][0] if i + 1 < len(lst) else sec_end
                    data_ranges.append((addr, end))
        data_ranges.sort()

        segments = [] if relocatable else [
            Segment(s['p_vaddr'], s.data(), s['p_memsz'], bool(s['p_flags'] & 2))
            for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD'
        ]

    functions.sort(key=lambda fn: fn.addr)
    for prev, cur in zip(functions, functions[1:]):
        if prev.addr == cur.addr:
            raise LoadError(
                f'{prev.name} and {cur.name} both live at {cur.addr:08x}. '
                'Objects with several code sections (e.g. -ffunction-sections) '
                'need linking first.')
    return Program(functions, segments, data_symbols, data_ranges)
