"""Read function symbols and their code bytes from an ARM ELF file."""

from dataclasses import dataclass

from elftools.elf.elffile import ELFFile
from elftools.elf.sections import SymbolTableSection


class LoadError(Exception):
    pass


@dataclass
class Function:
    name: str
    addr: int       # guest address (bit 0 cleared)
    code: bytes
    thumb: bool     # symbol value had bit 0 set => Thumb code


def load_functions(path):
    """Return every sized function symbol in the ELF, sorted by address."""
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

        functions = []
        for sym in symtab.iter_symbols():
            # ARM "mapping symbols" ($a, $t, $d) mark code/data regions; they
            # are STT_NOTYPE, so the STT_FUNC filter already drops them.
            if sym['st_info']['type'] != 'STT_FUNC' or sym['st_size'] == 0:
                continue
            shndx = sym['st_shndx']
            if not isinstance(shndx, int):      # SHN_UNDEF, SHN_ABS, ...
                continue

            section = elf.get_section(shndx)
            value = sym['st_value']
            # Bit 0 of a function symbol is the Thumb bit, not part of the address.
            addr = value & ~1
            # In a relocatable .o, st_value is an offset into its section;
            # in a linked executable it is a virtual address.
            offset = addr if relocatable else addr - section['sh_addr']
            code = section.data()[offset:offset + sym['st_size']]
            functions.append(Function(sym.name, addr, code, bool(value & 1)))

    functions.sort(key=lambda fn: fn.addr)
    for prev, cur in zip(functions, functions[1:]):
        if prev.addr == cur.addr:
            raise LoadError(
                f'{prev.name} and {cur.name} both live at {cur.addr:08x}. '
                'Objects with several code sections (e.g. -ffunction-sections) '
                'need linking first.')
    return functions
