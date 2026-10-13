"""Build a CRO module (3dbrew: CRO0) from an ARM object file, for testing
our CRO loader (hle/ro.c) and recompiler (recomp --cro) on code we wrote.

    python tests/m13/mkcro.py module.o NAME out.cro

Segments: 0 text, 1 rodata (every .rodata* section), 2 data, 3 bss. Every
R_ARM_ABS32 relocation becomes a patch, and the word it patches is zeroed
(as in real CROs):
  - against a symbol this object defines: an internal patch (target segment
    + offset);
  - against an undefined symbol: a named import of that symbol.
Every global symbol the object defines is a named export.
"""

import struct
import sys

from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection

R_ARM_ABS32 = 2
SEG_TEXT, SEG_RODATA, SEG_DATA, SEG_BSS = 0, 1, 2, 3


def align(n, a):
    return (n + a - 1) & ~(a - 1)


def build(obj_path, name):
    with open(obj_path, 'rb') as f:
        elf = ELFFile(f)
        sections = list(elf.iter_sections())

        # Where each object section goes: (segment, offset in segment).
        place, contents = {}, {SEG_TEXT: bytearray(), SEG_RODATA: bytearray(), SEG_DATA: bytearray()}
        bss_size = 0
        for i, sec in enumerate(sections):
            n = sec.name
            if n == '.text' or n.startswith('.text.'):
                seg = SEG_TEXT
            elif n.startswith('.rodata'):
                seg = SEG_RODATA
            elif n == '.data' or n.startswith('.data.'):
                seg = SEG_DATA
            elif n == '.bss' or n.startswith('.bss.'):
                bss_size = align(bss_size, max(sec['sh_addralign'], 1))
                place[i] = (SEG_BSS, bss_size)
                bss_size += sec['sh_size']
                continue
            else:
                continue
            buf = contents[seg]
            buf.extend(bytes(align(len(buf), max(sec['sh_addralign'], 4)) - len(buf)))
            place[i] = (seg, len(buf))
            buf.extend(sec.data())

        symtab = elf.get_section_by_name('.symtab')
        symbols = list(symtab.iter_symbols())
        exports = {}
        for sym in symbols:
            if sym['st_info']['bind'] == 'STB_GLOBAL' and isinstance(sym['st_shndx'], int) \
                    and sym['st_shndx'] in place:
                seg, off = place[sym['st_shndx']]
                exports[sym.name] = (seg, off + sym['st_value'])

        internal, imports = [], {}
        for sec in sections:
            if not isinstance(sec, RelocationSection):
                continue
            target = sec['sh_info']
            if target not in place:
                continue
            seg, base = place[target]
            for rel in sec.iter_relocations():
                kind = rel['r_info_type']
                if kind != R_ARM_ABS32:
                    raise SystemExit(f'{obj_path}: relocation type {kind} in {sec.name}: only R_ARM_ABS32')
                at = base + rel['r_offset']
                buf = contents[seg]
                inplace = struct.unpack_from('<i', buf, at)[0]       # REL: the addend is in place
                struct.pack_into('<I', buf, at, 0)
                sym = symbols[rel['r_info_sym']]
                if sym['st_shndx'] == 'SHN_UNDEF':
                    imports.setdefault(sym.name, []).append((seg, at, inplace))
                else:
                    tseg, toff = place[sym['st_shndx']]
                    internal.append((seg, at, tseg, toff + sym['st_value'] + inplace))

    # ---- lay the file out
    out = bytearray(0x180)                              # hashes (unchecked) + header
    seg_file = {}
    for seg in (SEG_TEXT, SEG_RODATA, SEG_DATA):
        out.extend(bytes(align(len(out), 0x10) - len(out)))
        seg_file[seg] = len(out)
        out.extend(contents[seg])

    def table(data, alignment=4):
        out.extend(bytes(align(len(out), alignment) - len(out)))
        at = len(out)
        out.extend(data)
        return at

    def segoff(seg, off):
        return seg | off << 4

    name_off = table(name.encode() + b'\0')
    segments = [(seg_file[SEG_TEXT], len(contents[SEG_TEXT]), SEG_TEXT),
                (seg_file[SEG_RODATA], len(contents[SEG_RODATA]), SEG_RODATA),
                (seg_file[SEG_DATA], len(contents[SEG_DATA]), SEG_DATA),
                (0, bss_size, SEG_BSS)]
    seg_table = table(b''.join(struct.pack('<III', *s) for s in segments))
    strings = bytearray()
    named_exports = []
    for sym, (seg, off) in sorted(exports.items()):
        named_exports.append((len(strings), segoff(seg, off)))
        strings.extend(sym.encode() + b'\0')
    export_strings = table(bytes(strings))
    export_table = table(b''.join(struct.pack('<II', export_strings + s, so) for s, so in named_exports))

    istrings = bytearray()
    import_names = []
    for sym in sorted(imports):
        import_names.append((sym, len(istrings)))
        istrings.extend(sym.encode() + b'\0')
    import_strings = table(bytes(istrings))
    patches = bytearray()
    named_imports = []
    for sym, soff in import_names:
        head = len(patches)
        entries = imports[sym]
        for k, (seg, at, addend) in enumerate(entries):
            last = int(k == len(entries) - 1)
            patches.extend(struct.pack('<IBBBBi', segoff(seg, at), R_ARM_ABS32, last, 0, 0, addend))
        named_imports.append((import_strings + soff, head))
    import_patches = table(bytes(patches))
    named_import_table = table(b''.join(struct.pack('<II', s, import_patches + h) for s, h in named_imports))
    internal_table = table(b''.join(struct.pack('<IBBBBi', segoff(seg, at), R_ARM_ABS32, tseg, 0, 0, addend)
                                    for seg, at, tseg, addend in internal))
    empty = table(b'')
    out.extend(b'\xcc' * (align(len(out), 0x1000) - len(out)))

    h = {
        0x80: int.from_bytes(b'CRO0', 'little'), 0x84: name_off, 0x88: 0, 0x8C: 0,
        0x90: len(out), 0x94: bss_size, 0x98: 0, 0x9C: 0,
        0xA0: 0xFFFFFFFF, 0xA4: 0xFFFFFFFF, 0xA8: 0xFFFFFFFF, 0xAC: 0xFFFFFFFF,
        0xB0: seg_file[SEG_TEXT], 0xB4: len(contents[SEG_TEXT]),
        0xB8: seg_file[SEG_DATA], 0xBC: len(contents[SEG_DATA]),
        0xC0: name_off, 0xC4: len(name) + 1,
        0xC8: seg_table, 0xCC: len(segments),
        0xD0: export_table, 0xD4: len(named_exports),
        0xD8: empty, 0xDC: 0,
        0xE0: export_strings, 0xE4: len(strings),
        0xE8: empty, 0xEC: 0,
        0xF0: empty, 0xF4: 0,
        0xF8: import_patches, 0xFC: len(patches) // 12,
        0x100: named_import_table, 0x104: len(named_imports),
        0x108: empty, 0x10C: 0, 0x110: empty, 0x114: 0,
        0x118: import_strings, 0x11C: len(istrings),
        0x120: empty, 0x124: 0,
        0x128: internal_table, 0x12C: len(internal),
        0x130: empty, 0x134: 0,
    }
    for off, value in h.items():
        struct.pack_into('<I', out, off, value & 0xFFFFFFFF)
    return bytes(out)


def main():
    obj, name, dest = sys.argv[1:4]
    open(dest, 'wb').write(build(obj, name))
    return 0


if __name__ == '__main__':
    sys.exit(main())
