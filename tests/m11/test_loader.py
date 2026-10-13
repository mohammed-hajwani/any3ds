"""Tests for recomp/ctr_loader.py: exheader fields, code.bin decompression,
NCCH/NCSD containers, and refusing encrypted images.

    python tests/m11/test_loader.py
"""

import random
import struct
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(Path(__file__).parent))

from mkcode import exheader, lz_compress, ncch  # noqa: E402
from recomp.ctr_loader import load_ctr, lz_decompress, parse_exheader, read_ctr  # noqa: E402
from recomp.elf_loader import LoadError  # noqa: E402

checks = failed = 0


def check(cond, what):
    global checks, failed
    checks += 1
    if not cond:
        failed += 1
        print(f'FAIL: {what}')


def raises(fn, text):
    try:
        fn()
    except LoadError as e:
        return text in str(e)
    return False


# ---- decompression: hand-made streams, decoded by hand

# "abc" * 8 (24 bytes), decoded from the end: literals c, b, a (positions
# 23-21), then an 18-byte copy at distance 3 (positions 20-3), then a 3-byte
# copy (2-0). Flags 0b00011000; tokens (length-3)<<12 | (distance-3).
# Reading goes backwards, so the stored order is reversed: M3, M18, a, b, c, flags.
body = bytes([0x00, 0x00, 0x00, 0xF0, ord('a'), ord('b'), ord('c'), 0x18])
comp = body + struct.pack('<II', 8 << 24 | (len(body) + 8), 24 - (len(body) + 8))
check(lz_decompress(comp) == b'abc' * 8, 'hand-made stream: literals and back-references')

# A stored prefix: everything below the compressed part comes through as is.
comp = b'xyz!' + body + struct.pack('<II', 8 << 24 | (len(body) + 8), 24 - (len(body) + 8))
check(lz_decompress(comp) == b'xyz!' + b'abc' * 8, 'hand-made stream: uncompressed prefix kept')

data = b'abcabcabcabc'
packed = lz_compress(data * 8)
check(packed is not None and len(packed) < len(data) * 8, 'repetitive data compresses')
check(lz_decompress(packed) == data * 8, 'round trip: repetitive data')

rng = random.Random(3)
for trial in range(20):
    n = rng.randrange(64, 6000)
    alphabet = rng.randrange(2, 40)
    blob = bytes(rng.randrange(alphabet) for _ in range(n))
    if trial % 4 == 0:      # long-distance repeats
        blob = blob[: n // 2] + bytes(rng.randrange(256) for _ in range(rng.randrange(0, 4000))) + blob[: n // 2]
    packed = lz_compress(blob)
    if packed is not None:
        check(lz_decompress(packed) == blob, f'round trip #{trial} ({len(blob)} bytes)')

noise = random.Random(1)
check(lz_compress(bytes(noise.randrange(256) for _ in range(500))) is None,
      'incompressible data is left uncompressed')
check(raises(lambda: lz_decompress(b'\0' * 4), 'too short'), 'too-short input is refused')
bad = bytearray(lz_compress(data * 8))
struct.pack_into('<I', bad, len(bad) - 8, 0xFF << 24 | 0xFFFFFF)
check(raises(lambda: lz_decompress(bytes(bad)), 'footer'), 'a bad footer is refused')

# ---- exheader

ex = parse_exheader(exheader('demo', True, [(0x100000, 3, 0x2345), (0x103000, 1, 0x10), (0x104000, 2, 0x1800)],
                             0x4000, 0x900))
check(ex.title == 'demo' and ex.compressed, 'exheader: title and compression flag')
check((ex.text.addr, ex.text.pages, ex.text.size) == (0x100000, 3, 0x2345), 'exheader: text code set')
check((ex.data.addr, ex.data.size) == (0x104000, 0x1800), 'exheader: data code set')
check(ex.stack_size == 0x4000 and ex.bss_size == 0x900, 'exheader: stack and bss sizes')

# ---- whole programs: directory, NCCH, NCSD

text = bytes(range(256)) * 16 + b'\x1e\xff\x2f\xe1'               # 4 KiB, then `bx lr`
rodata, datas = b'hello\0', b'\x01\x02\x03\x04'
code = text + bytes(0x2000 - len(text)) + rodata + bytes(0x1000 - len(rodata)) + datas
sets = [(0x100000, 2, len(text)), (0x102000, 1, len(rodata)), (0x103000, 1, len(datas))]
packed = lz_compress(code)
raw_ex = exheader('demo', True, sets, 0x4000, 0x100)
with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    (tmp / 'exheader.bin').write_bytes(raw_ex)
    (tmp / 'code.bin').write_bytes(packed)
    p = load_ctr(tmp)
    check([(s.vaddr, s.data, s.memsz, s.writable) for s in p.segments] ==
          [(0x100000, text, len(text), False), (0x102000, rodata, len(rodata), False),
           (0x103000, datas, len(datas) + 0x100, True)], 'directory: segments, bss and permissions')
    check(p.entry == 0x100000 and p.functions == [], 'directory: entry point, no symbols')

    cxi = tmp / 'demo.cxi'
    cxi.write_bytes(ncch(raw_ex, packed))
    _, got, info = read_ctr(cxi)
    check(got == code and info['product_code'] == 'CTR-P-TEST', 'NCCH: .code found and decompressed')

    ncsd = bytearray(0x4000)
    ncsd[0x100:0x104] = b'NCSD'
    struct.pack_into('<II', ncsd, 0x120, 0x4000 // 0x200, cxi.stat().st_size // 0x200)
    cci = tmp / 'demo.3ds'
    cci.write_bytes(bytes(ncsd) + cxi.read_bytes())
    _, got, _ = read_ctr(cci)
    check(got == code, 'NCSD: the first partition is read')

    enc = bytearray(cxi.read_bytes())
    enc[0x18F] = 0                                              # NoCrypto cleared: encrypted
    (tmp / 'enc.cxi').write_bytes(enc)
    check(raises(lambda: read_ctr(tmp / 'enc.cxi'), 'encrypted'), 'an encrypted image is refused')

    short = tmp / 'short'
    short.mkdir()
    (short / 'exheader.bin').write_bytes(exheader('demo', False, sets, 0x4000, 0))
    (short / 'code.bin').write_bytes(code[:0x1000])
    check(raises(lambda: load_ctr(short), 'too short'), 'a truncated code.bin is refused')

print(f'{"PASS" if not failed else "FAIL"}: {checks} checks, {failed} failed')
sys.exit(1 if failed else 0)
