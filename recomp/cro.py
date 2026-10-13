"""Parse CRO modules (the 3DS's "DLLs") and the CRS that describes the main
program, from the format on 3dbrew (CRO0).

A CRO is laid out in segments (text, rodata, data, bss), and almost every
cross-reference in it is a "segment offset": bits 0-3 the segment index,
bits 4-31 an offset into that segment. When the loader (ldr:ro) maps a
module it turns those into addresses and applies its relocation patches;
the recompiler reads the same tables to know what the loader will patch.

    python -m recomp.cro DUMP cro/DllField.cro      summarise a module
"""

import struct
import sys
from dataclasses import dataclass, field

from .elf_loader import LoadError

SEG_TEXT, SEG_RODATA, SEG_DATA, SEG_BSS = 0, 1, 2, 3
SEG_NAMES = {SEG_TEXT: 'text', SEG_RODATA: 'rodata', SEG_DATA: 'data', SEG_BSS: 'bss'}

R_ARM_NONE, R_ARM_ABS32, R_ARM_REL32, R_ARM_THM_PC22 = 0, 2, 3, 10
R_ARM_CALL, R_ARM_JUMP24, R_ARM_TARGET1, R_ARM_PREL31 = 28, 29, 38, 42

# Header fields (offsets into the file; the header proper starts at 0x80).
_FIELDS = {
    'magic': 0x80, 'name_offset': 0x84, 'file_size': 0x90, 'bss_size': 0x94,
    'control_object': 0xA0, 'on_load': 0xA4, 'on_exit': 0xA8, 'on_unresolved': 0xAC,
    'code_offset': 0xB0, 'code_size': 0xB4, 'data_offset': 0xB8, 'data_size': 0xBC,
    'module_name_offset': 0xC0, 'module_name_size': 0xC4,
    'segment_table': 0xC8, 'segment_num': 0xCC,
    'named_export_table': 0xD0, 'named_export_num': 0xD4,
    'indexed_export_table': 0xD8, 'indexed_export_num': 0xDC,
    'export_strings': 0xE0, 'export_strings_size': 0xE4,
    'export_trie': 0xE8, 'export_trie_num': 0xEC,
    'import_module_table': 0xF0, 'import_module_num': 0xF4,
    'import_patches': 0xF8, 'import_patch_num': 0xFC,
    'named_import_table': 0x100, 'named_import_num': 0x104,
    'indexed_import_table': 0x108, 'indexed_import_num': 0x10C,
    'anonymous_import_table': 0x110, 'anonymous_import_num': 0x114,
    'import_strings': 0x118, 'import_strings_size': 0x11C,
    'static_anonymous_symbols': 0x120, 'static_anonymous_num': 0x124,
    'internal_patches': 0x128, 'internal_patch_num': 0x12C,
    'static_patches': 0x130, 'static_patch_num': 0x134,
}
HEADER_SIZE = 0x138
NO_OFFSET = 0xFFFFFFFF


@dataclass
class Segment:
    index: int
    offset: int             # file offset (for bss: none, the loader allocates it)
    size: int
    kind: int               # SEG_TEXT / RODATA / DATA / BSS


@dataclass
class Patch:
    """A relocation the loader applies: write `kind` of (target + addend)
    at segment offset `at`."""
    at: tuple               # (segment index, offset) to patch
    kind: int               # R_ARM_*
    target_segment: int     # internal patches: the segment the target is in
    addend: int


@dataclass
class Import:
    name: str               # symbol name, or "module#index" / "module@segoff" for indexed/anonymous
    patches: list           # [Patch]


@dataclass
class Module:
    name: str
    data: bytes             # the whole file
    segments: list
    named_exports: dict     # name -> (segment, offset)
    indexed_exports: list   # [(segment, offset)]
    imports: list           # [Import]
    import_modules: list    # names of the modules imported from
    internal_patches: list  # [Patch]
    static_patches: list    # [Patch] (field 0x130: purpose unknown)
    on_load: tuple = None
    on_exit: tuple = None
    header: dict = field(default_factory=dict)

    def segment(self, index):
        return self.segments[index]

    def file_offset(self, segoff):
        seg, off = segoff
        return self.segments[seg].offset + off


def split(segoff):
    """A segment offset -> (segment index, offset)."""
    return segoff & 0xF, segoff >> 4


def _cstring(data, at):
    end = data.index(b'\0', at)
    return data[at:end].decode('ascii', 'replace')


def _patch_list(data, at, num, internal):
    patches = []
    for i in range(num):
        segoff, kind, seg_or_last, _, _, addend = struct.unpack_from('<IBBBBi', data, at + 12 * i)
        patches.append(Patch(split(segoff), kind, seg_or_last if internal else None, addend))
    return patches


def _import_patches(data, h, at):
    """An import's patch list: a run of entries in the import patch table,
    ending at the one flagged "last"."""
    base, num = h['import_patches'], h['import_patch_num']
    if not (base <= at < base + 12 * num):
        raise LoadError(f'import patch list at {at:#x} outside the table')
    patches = []
    while True:
        segoff, kind, last, _, _, addend = struct.unpack_from('<IBBBBi', data, at)
        patches.append(Patch(split(segoff), kind, None, addend))
        if last:
            return patches
        at += 12
        if at >= base + 12 * num:
            raise LoadError('import patch list runs off the table')


def parse(data):
    if len(data) < HEADER_SIZE or data[0x80:0x84] not in (b'CRO0', b'FIXD'):
        raise LoadError('not a CRO/CRS (no "CRO0" magic at 0x80)')
    h = {k: struct.unpack_from('<I', data, off)[0] for k, off in _FIELDS.items()}

    segments = []
    for i in range(h['segment_num']):
        off, size, kind = struct.unpack_from('<III', data, h['segment_table'] + 12 * i)
        segments.append(Segment(i, off, size, kind))

    named_exports = {}
    for i in range(h['named_export_num']):
        name_off, segoff = struct.unpack_from('<II', data, h['named_export_table'] + 8 * i)
        named_exports[_cstring(data, name_off)] = split(segoff)
    indexed_exports = [split(struct.unpack_from('<I', data, h['indexed_export_table'] + 4 * i)[0])
                       for i in range(h['indexed_export_num'])]

    imports = []
    for i in range(h['named_import_num']):
        name_off, patch_at = struct.unpack_from('<II', data, h['named_import_table'] + 8 * i)
        imports.append(Import(_cstring(data, name_off), _import_patches(data, h, patch_at)))
    import_modules = []
    for i in range(h['import_module_num']):
        mod_name_off, idx_head, idx_num, anon_head, anon_num = struct.unpack_from(
            '<5I', data, h['import_module_table'] + 20 * i)
        mod = _cstring(data, mod_name_off)
        import_modules.append(mod)
        for k in range(idx_num):
            index, patch_at = struct.unpack_from('<II', data, idx_head + 8 * k)
            imports.append(Import(f'{mod}#{index}', _import_patches(data, h, patch_at)))
        for k in range(anon_num):
            segoff, patch_at = struct.unpack_from('<II', data, anon_head + 8 * k)
            seg, off = split(segoff)
            imports.append(Import(f'{mod}@{seg}:{off:x}', _import_patches(data, h, patch_at)))

    def hook(segoff):
        return None if segoff == NO_OFFSET else split(segoff)

    name = data[h['module_name_offset']:h['module_name_offset'] + h['module_name_size']].rstrip(b'\0')
    return Module(
        name=name.decode('ascii', 'replace'),
        data=data,
        segments=segments,
        named_exports=named_exports,
        indexed_exports=indexed_exports,
        imports=imports,
        import_modules=import_modules,
        internal_patches=_patch_list(data, h['internal_patches'], h['internal_patch_num'], True),
        static_patches=_patch_list(data, h['static_patches'], h['static_patch_num'], True),
        on_load=hook(h['on_load']),
        on_exit=hook(h['on_exit']),
        header=h,
    )


# ---------------------------------------------------------------- the recompiler's view of a module
#
# In the file, every word the loader patches is zero. To find the code we
# patch them ourselves, with the module placed at 0: text and rodata sit at
# their file offsets (as the loader maps the whole file at the base the
# game chooses), and data and bss, which the loader moves into buffers of
# the game's, at addresses far from the code so nothing mistakes them for it.
# Generated code then computes code addresses as RB + offset (translate.py,
# RELOCATABLE), RB being the base the module was really loaded at.

FAKE_SEGMENT_BASE = 0x70000000      # data/bss segment i -> FAKE_SEGMENT_BASE + i << 24


def segment_address(module, index):
    seg = module.segments[index]
    if seg.kind in (SEG_TEXT, SEG_RODATA):
        return seg.offset
    return FAKE_SEGMENT_BASE + (index << 24)


def relocated_program(module):
    """(Program for discovery and translation, roots) for a CRO module."""
    from .elf_loader import Program, Segment as ProgSegment
    data = bytearray(module.data)
    patched = set()
    pointers = set()
    for p in module.internal_patches:
        if p.kind not in (R_ARM_ABS32, R_ARM_TARGET1):
            raise LoadError(f'{module.name}: internal relocation type {p.kind} is not implemented yet')
        at = module.file_offset(p.at)
        value = (segment_address(module, p.target_segment) + p.addend) & 0xFFFFFFFF
        struct.pack_into('<I', data, at, value)
        patched.add(at)
        if module.segments[p.target_segment].kind == SEG_TEXT:
            pointers.add(value)
    for imp in module.imports:
        for p in imp.patches:
            if p.kind not in (R_ARM_ABS32, R_ARM_TARGET1):
                raise LoadError(f'{module.name}: import relocation type {p.kind} is not implemented yet')
            patched.add(module.file_offset(p.at))     # filled in when the module is linked

    segments, text = [], None
    for s in module.segments:
        if not s.size:
            continue
        if s.kind == SEG_BSS:
            continue
        addr = segment_address(module, s.index)
        seg = ProgSegment(addr, bytes(data[s.offset:s.offset + s.size]), s.size, s.kind == SEG_DATA)
        segments.append(seg)
        if s.kind == SEG_TEXT:
            if text is not None:
                raise LoadError(f'{module.name}: more than one text segment')
            text = seg
    if text is None:
        raise LoadError(f'{module.name}: no text segment')

    program = Program([], segments, {}, [], [], None)
    program.text_segment = text
    program.pointer_candidates = pointers
    program.patched = patched
    roots = []
    hooks = [module.on_load, module.on_exit] + list(module.named_exports.values()) + module.indexed_exports
    for hook in hooks:
        if hook and module.segments[hook[0]].kind == SEG_TEXT:
            value = segment_address(module, hook[0]) + hook[1]
            roots.append((value & ~1, bool(value & 1)))
    return program, roots


def without_patched(ranges, patched):
    """Literal-pool ranges minus the words the loader patches: those must be
    read from memory at run time, never folded into the code."""
    out = []
    for start, end in ranges:
        if not any(a in patched for a in range(start & ~3, end, 4)):
            out.append((start, end))
    return out


def main(argv=None):
    from .romfs import RomFS
    argv = sys.argv[1:] if argv is None else argv
    m = parse(open(argv[0], "rb").read() if argv[0].endswith(".cro") else RomFS(argv[0]).read(argv[1]))
    print(f'module {m.name}: {len(m.data):#x} bytes')
    for s in m.segments:
        print(f'  segment {s.index}: {SEG_NAMES.get(s.kind, s.kind):6} file offset {s.offset:#8x}, {s.size:#8x} bytes')
    print(f'  exports: {len(m.named_exports)} named, {len(m.indexed_exports)} indexed')
    print(f'  imports: {len(m.imports)} from {len(m.import_modules)} modules {m.import_modules[:6]}')
    kinds = {}
    for p in m.internal_patches:
        kinds[p.kind] = kinds.get(p.kind, 0) + 1
    print(f'  internal patches: {len(m.internal_patches)} {kinds}')
    ikinds = {}
    for imp in m.imports:
        for p in imp.patches:
            ikinds[p.kind] = ikinds.get(p.kind, 0) + 1
    print(f'  import patches: {sum(len(i.patches) for i in m.imports)} {ikinds}')
    print(f'  static patches (0x130): {len(m.static_patches)}')
    print(f'  OnLoad {m.on_load}, OnExit {m.on_exit}')
    for name in list(m.named_exports)[:8]:
        print(f'    export {name} -> {m.named_exports[name]}')


if __name__ == '__main__':
    main()
