"""Turn a linked homebrew ELF into what a retail program ships as: a
`code.bin` (text, rodata and data, each padded to whole pages) and an
`exheader.bin` saying where each part goes. No symbols survive, so the
recompiler has to find every function by itself.

    python tests/m11/mkcode.py program.elf OUT_DIR [--compress] [--ncch FILE]

--compress  compresses code.bin the way ExeFS `.code` is ("reverse LZSS"),
            so the loader's decompressor gets exercised.
--ncch      also wraps everything in a minimal decrypted NCCH (.cxi).
"""

import argparse
import struct
import sys
from pathlib import Path

from elftools.elf.elffile import ELFFile

PAGE = 0x1000


def pages(n):
    return (n + PAGE - 1) // PAGE


def lz_compress(data):
    """Compress so that recomp.ctr_loader.lz_decompress gives `data` back.

    Works from the end of the data towards its start, as the decompressor
    does. Each item is a literal byte or a back-reference of 3-18 bytes at
    a distance of 3-4098 (towards the end, into bytes already produced).
    Returns None if that wouldn't make it smaller.
    """
    n = len(data)
    buf = bytearray(n + n // 8 + 16)
    w = len(buf)
    recent = {}                 # 3-byte key -> positions q (key = data[q-2:q+1]) already passed
    p = n                       # bytes [0, p) still to encode
    ctrl_at, bit = None, 8
    while p > 0:
        if bit == 8:
            w -= 1
            ctrl_at, bit = w, 0
            buf[ctrl_at] = 0
        q = p - 1
        best_len, best_dist = 0, 0
        if q >= 2:
            key = bytes(data[q - 2:q + 1])
            for r in reversed(recent.get(key, [])[-32:]):
                d = r - q
                if d > 4098:
                    continue
                if d < 3:
                    continue
                length = 0
                while length < 18 and q - length >= 0 and data[q - length] == data[q - length + d]:
                    length += 1
                if length > best_len:
                    best_len, best_dist = length, d
                    if length == 18:
                        break
        if best_len >= 3:
            token = (best_len - 3) << 12 | (best_dist - 3)
            w -= 2
            buf[w], buf[w + 1] = token & 0xFF, token >> 8
            buf[ctrl_at] |= 0x80 >> bit
            step = best_len
        else:
            w -= 1
            buf[w] = data[q]
            step = 1
        for k in range(step):
            pos = q - k
            if pos >= 2:
                recent.setdefault(bytes(data[pos - 2:pos + 1]), []).append(pos)
        p -= step
        bit += 1
    enc = bytes(buf[w:])
    pad = -len(enc) % 4
    total = len(enc) + pad + 8
    if total >= n:
        return None
    footer = struct.pack('<II', (8 + pad) << 24 | total, n - total)
    return enc + bytes(pad) + footer


def exheader(title, compressed, sets, stack_size, bss_size):
    """The system control info part of an exheader (the rest stays zero)."""
    raw = bytearray(0x400)
    raw[0:8] = title.encode()[:8].ljust(8, b'\0')
    raw[0xD] = 1 if compressed else 0
    for off, (addr, npages, size) in zip((0x10, 0x20, 0x30), sets):
        struct.pack_into('<III', raw, off, addr, npages, size)
    struct.pack_into('<I', raw, 0x1C, stack_size)
    struct.pack_into('<I', raw, 0x3C, bss_size)
    return bytes(raw)


def ncch(exheader_raw, code, program_id=0x000400000FF3D000):
    """A minimal decrypted NCCH: header, exheader, and an ExeFS with .code."""
    exefs = bytearray(0x200)
    exefs[0:8] = b'.code'.ljust(8, b'\0')
    struct.pack_into('<II', exefs, 8, 0, len(code))
    exefs += code
    exefs += bytes(-len(exefs) % 0x200)
    hdr = bytearray(0x200)
    hdr[0x100:0x104] = b'NCCH'
    struct.pack_into('<Q', hdr, 0x108, program_id)
    struct.pack_into('<Q', hdr, 0x118, program_id)
    hdr[0x150:0x160] = b'CTR-P-TEST'.ljust(0x10, b'\0')
    struct.pack_into('<I', hdr, 0x180, len(exheader_raw))
    hdr[0x18F] = 0x4                            # NoCrypto
    exefs_off = 0x200 + len(exheader_raw)
    struct.pack_into('<II', hdr, 0x1A0, exefs_off // 0x200, len(exefs) // 0x200)
    struct.pack_into('<I', hdr, 0x104, (exefs_off + len(exefs)) // 0x200)
    return bytes(hdr) + exheader_raw + bytes(exefs)


def from_elf(path, compress):
    with open(path, 'rb') as f:
        elf = ELFFile(f)
        segs = [s for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
        if len(segs) != 3:
            raise SystemExit(f'{path}: expected 3 loadable segments (text, rodata, data), found {len(segs)}')
        sets, code, bss = [], bytearray(), 0
        for s in segs:
            if s['p_vaddr'] % PAGE:
                raise SystemExit(f'{path}: segment at {s["p_vaddr"]:08x} is not page-aligned')
            data = s.data()
            sets.append((s['p_vaddr'], pages(len(data)), len(data)))
            code += data + bytes(pages(len(data)) * PAGE - len(data))
        bss = segs[2]['p_memsz'] - segs[2]['p_filesz']
    raw = bytes(code)
    packed = lz_compress(raw) if compress else None
    ex = exheader('any3ds', packed is not None, sets, 0x8000, bss)
    return ex, packed if packed is not None else raw


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('elf')
    ap.add_argument('out', type=Path)
    ap.add_argument('--compress', action='store_true')
    ap.add_argument('--ncch', type=Path)
    args = ap.parse_args()
    ex, code = from_elf(args.elf, args.compress)
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / 'exheader.bin').write_bytes(ex)
    (args.out / 'code.bin').write_bytes(code)
    if args.ncch:
        args.ncch.write_bytes(ncch(ex, code))
    return 0


if __name__ == '__main__':
    sys.exit(main())
