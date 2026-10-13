"""Read files out of a 3DS RomFS (3dbrew: RomFS), e.g. a game's CRO modules
(`cro/*.cro`, `cro/static.crs`). The RomFS is found inside a decrypted
NCCH (.cxi) or the first partition of a decrypted .3ds, through its IVFC
hash tree; only level 3 (the file system itself) is read.

    python -m recomp.romfs DUMP [DIR]        list the files (under DIR)
"""

import mmap
import struct
import sys

from .elf_loader import LoadError

NONE = 0xFFFFFFFF


def _level3(data):
    """Offset of the RomFS level 3 inside a decrypted .cxi/.3ds."""
    ncch = 0
    if data[0x100:0x104] == b'NCSD':
        ncch = struct.unpack_from('<I', data, 0x120)[0] * 0x200
    if data[ncch + 0x100:ncch + 0x104] != b'NCCH':
        raise LoadError('not an NCCH/NCSD image')
    if not data[ncch + 0x18F] & 4:
        raise LoadError('this image is encrypted; decrypt your own dump on your console first')
    ivfc = ncch + struct.unpack_from('<I', data, ncch + 0x1B0)[0] * 0x200
    if data[ivfc:ivfc + 4] != b'IVFC':
        raise LoadError('no RomFS (IVFC) in this NCCH')
    master = struct.unpack_from('<I', data, ivfc + 0x08)[0]
    block = 1 << struct.unpack_from('<I', data, ivfc + 0x4C)[0]
    return ivfc + ((0x60 + master + block - 1) & ~(block - 1))


class RomFS:
    def __init__(self, path):
        f = open(path, 'rb')
        self._map = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        f.close()
        self.base = _level3(self._map)
        h = struct.unpack_from('<10I', self._map, self.base)
        self.dir_meta = self.base + h[3]
        self.file_meta = self.base + h[7]
        self.file_data = self.base + h[9]

    def _name(self, at, length):
        return bytes(self._map[at:at + length]).decode('utf-16-le')

    def _dir(self, off):
        p = self.dir_meta + off
        parent, sibling, child, first_file, _, name_len = struct.unpack_from('<6I', self._map, p)
        return sibling, child, first_file, self._name(p + 24, name_len)

    def _file(self, off):
        p = self.file_meta + off
        parent, sibling, data_off, size, _, name_len = struct.unpack_from('<IIQQII', self._map, p)
        return sibling, data_off, size, self._name(p + 32, name_len)

    def walk(self, off=0, prefix=''):
        """Yields (path, data offset, size) for every file under directory `off`."""
        sibling, child, f, _ = self._dir(off)
        while f != NONE:
            f_sib, data_off, size, name = self._file(f)
            yield prefix + name, data_off, size
            f = f_sib
        while child != NONE:
            c_sib, _, _, name = self._dir(child)
            yield from self.walk(child, prefix + name + '/')
            child = c_sib

    def read(self, path):
        for p, off, size in self.walk():
            if p == path:
                return bytes(self._map[self.file_data + off:self.file_data + off + size])
        raise LoadError(f'RomFS has no file {path}')


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    fs = RomFS(argv[0])
    under = argv[1] if len(argv) > 1 else ''
    total = 0
    for path, _, size in fs.walk():
        if path.startswith(under):
            print(f'{size:10}  {path}')
            total += size
    print(f'{total:10}  total')


if __name__ == '__main__':
    main()
