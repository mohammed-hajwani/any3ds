"""Read a 3DS program the way the console's loader sees it: `code.bin`
(text, rodata and data, one after another) plus the exheader that says
where each goes. Accepted inputs:

  - a directory holding `code.bin` and `exheader.bin`;
  - a decrypted NCCH (`.cxi`);
  - a decrypted cartridge image (`.3ds`, NCSD): its first partition.

Only decrypted images are read (the NCCH "NoCrypto" flag). We never handle
keys: decrypt your own dump on your console first.

Formats: 3dbrew's NCCH, NCCH/Extended_Header and ExeFS pages.
"""

import struct
from dataclasses import dataclass
from pathlib import Path

from .elf_loader import LoadError, Program, Segment

MEDIA_UNIT = 0x200
PAGE = 0x1000


@dataclass
class CodeSet:
    addr: int
    pages: int              # pages reserved for it
    size: int               # bytes actually used


@dataclass
class Exheader:
    title: str
    compressed: bool        # code.bin is reverse-LZSS compressed
    text: CodeSet
    rodata: CodeSet
    data: CodeSet
    stack_size: int
    bss_size: int


def parse_exheader(raw):
    if len(raw) < 0x200:
        raise LoadError('exheader: shorter than its 0x200-byte system control info')

    def code_set(off):
        return CodeSet(*struct.unpack_from('<III', raw, off))

    return Exheader(
        title=raw[0:8].rstrip(b'\0').decode('ascii', 'replace'),
        compressed=bool(raw[0xD] & 1),
        text=code_set(0x10),
        rodata=code_set(0x20),
        data=code_set(0x30),
        stack_size=struct.unpack_from('<I', raw, 0x1C)[0],
        bss_size=struct.unpack_from('<I', raw, 0x3C)[0],
    )


def lz_decompress(comp):
    """Undo the ExeFS `.code` compression ("reverse LZSS").

    The data is decompressed from its end towards its start. The last 8
    bytes are a footer: a word whose low 24 bits are the size of the
    compressed part (counted back from the end of the buffer) and whose top
    8 bits are the footer+padding length, then a word holding how many
    bytes decompression adds. Everything before the compressed part is
    stored as is. Reading backwards, each flag byte describes 8 items,
    most significant bit first: 0 = one literal byte, 1 = a 16-bit
    back-reference: bits 12-15 = length - 3, bits 0-11 = distance - 3,
    where the distance counts back up from the byte being written to the
    already-written bytes above it.
    """
    if len(comp) < 8:
        raise LoadError('code.bin: too short to be compressed')
    top_bottom, extra = struct.unpack_from('<II', comp, len(comp) - 8)
    out = bytearray(comp) + bytes(extra)
    src = len(comp) - (top_bottom >> 24)
    stop = len(comp) - (top_bottom & 0xFFFFFF)
    dst = len(out)
    if not 0 <= stop <= src <= len(comp):
        raise LoadError('code.bin: bad compression footer')
    while src > stop:
        src -= 1
        flags = comp[src]
        for _ in range(8):
            if src <= stop:
                break
            if flags & 0x80:
                src -= 2
                if src < stop:
                    raise LoadError('code.bin: compressed data ends inside a back-reference')
                token = comp[src] | comp[src + 1] << 8
                length, dist = (token >> 12) + 3, (token & 0xFFF) + 3
                if dst < length:
                    raise LoadError('code.bin: decompresses past its start')
                for _ in range(length):
                    dst -= 1
                    if dst + dist >= len(out):
                        raise LoadError('code.bin: back-reference outside the data')
                    out[dst] = out[dst + dist]
            else:
                src -= 1
                dst -= 1
                if dst < 0:
                    raise LoadError('code.bin: decompresses past its start')
                out[dst] = comp[src]
            flags <<= 1
    if dst != stop:
        raise LoadError(f'code.bin: decompression ended at {dst:#x}, expected {stop:#x}')
    return bytes(out)


def _exefs_file(exefs, name):
    for i in range(10):
        raw_name, off, size = struct.unpack_from('<8sII', exefs, 16 * i)
        if raw_name.rstrip(b'\0') == name:
            return exefs[0x200 + off:0x200 + off + size]
    raise LoadError(f'ExeFS has no {name.decode()} file')


def _read_ncch(f, base):
    """(exheader bytes, code.bin bytes, info) from the NCCH at offset `base`."""
    f.seek(base)
    hdr = f.read(0x200)
    if hdr[0x100:0x104] != b'NCCH':
        raise LoadError('not an NCCH (no NCCH magic)')
    flags = hdr[0x188:0x190]
    if not flags[7] & 0x4:
        raise LoadError('this image is encrypted. Decrypt your own dump on your console first '
                        '(we never handle keys)')
    ex_size = struct.unpack_from('<I', hdr, 0x180)[0]
    exefs_off, exefs_size = (x * MEDIA_UNIT for x in struct.unpack_from('<II', hdr, 0x1A0))
    romfs_off, romfs_size = (x * MEDIA_UNIT for x in struct.unpack_from('<II', hdr, 0x1B0))
    if not ex_size or not exefs_size:
        raise LoadError('this NCCH has no exheader/ExeFS: it is not an executable (CXI)')
    f.seek(base + 0x200)
    exheader = f.read(ex_size)
    f.seek(base + exefs_off)
    exefs = f.read(exefs_size)
    info = {
        'program_id': struct.unpack_from('<Q', hdr, 0x118)[0],
        'product_code': hdr[0x150:0x160].rstrip(b'\0').decode('ascii', 'replace'),
        'romfs': (base + romfs_off, romfs_size) if romfs_size else None,
    }
    return exheader, _exefs_file(exefs, b'.code'), info


def read_ctr(path):
    """(Exheader, decompressed code.bin, info) for any accepted input."""
    path = Path(path)
    if path.is_dir():
        exheader = (path / 'exheader.bin').read_bytes()
        code = (path / 'code.bin').read_bytes()
        info = {'program_id': None, 'product_code': None, 'romfs': None}
    else:
        with open(path, 'rb') as f:
            head = f.read(0x200)
            if head[0x100:0x104] == b'NCSD':
                part0 = struct.unpack_from('<I', head, 0x120)[0] * MEDIA_UNIT
                exheader, code, info = _read_ncch(f, part0)
            else:
                exheader, code, info = _read_ncch(f, 0)
    ex = parse_exheader(exheader)
    if ex.compressed:
        code = lz_decompress(code)
    return ex, code, info


def load_ctr(path):
    """A `Program` with the three segments and no symbols at all."""
    ex, code, info = read_ctr(path)
    segments, off = [], 0
    for cs, writable, extra in ((ex.text, False, 0), (ex.rodata, False, 0), (ex.data, True, ex.bss_size)):
        if cs.size > cs.pages * PAGE:
            raise LoadError(f'exheader: a code set of {cs.size:#x} bytes does not fit in {cs.pages} pages')
        if cs.size:
            if off + cs.size > len(code):
                raise LoadError(f'code.bin is {len(code):#x} bytes, too short for its exheader')
            segments.append(Segment(cs.addr, code[off:off + cs.size], cs.size + extra, writable))
        off += cs.pages * PAGE
    if ex.text.addr != 0x00100000:
        raise LoadError(f'text starts at {ex.text.addr:08x}, not 00100000')
    program = Program([], segments, {}, [], [], ex.text.addr)
    program.exheader = ex
    program.info = info
    return program
