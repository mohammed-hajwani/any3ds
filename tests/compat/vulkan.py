#!/usr/bin/env python3
"""Compare the Vulkan renderer with the software one (make test-vulkan).

Every GPU example that runs in the compat sweep is run again with
GPU_BACKEND=vulkan, and its last frame is compared with the sweep's
software frame (out/compat/<example>/frame.bmp, so `make compat` must have
run). By default Vulkan is Mesa's lavapipe, a CPU driver, so this needs no
GPU; set VK_DRIVER_FILES to try another driver.

The two may differ by a step of rounding where blending is involved, and
where a pixel lies exactly on a triangle's edge, so a frame passes when no
channel is off by more than TOLERANCE and at most MAX_EDGE_PIXELS pixels
differ at all.
"""
import os
import re
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sweep  # noqa: E402

TOLERANCE = 2
MAX_EDGE_PIXELS = 0.002          # a fraction of the frame
LAVAPIPE = '/usr/share/vulkan/icd.d/lvp_icd.json'


def read_bmp(path):
    data = path.read_bytes()
    offset, = struct.unpack_from('<I', data, 10)
    width, height = struct.unpack_from('<ii', data, 18)
    bpp, = struct.unpack_from('<H', data, 28)
    return width, abs(height), bpp // 8, data[offset:]


def compare(a, b):
    """(pixels that differ, largest channel difference) of two dumps."""
    wa, ha, pa, da = read_bmp(a)
    wb, hb, pb, db = read_bmp(b)
    if (wa, ha, pa) != (wb, hb, pb) or len(da) != len(db):
        return None
    differ = worst = 0
    for i in range(0, min(len(da), wa * ha * pa), pa):
        d = max(abs(da[i + k] - db[i + k]) for k in range(pa))
        if d:
            differ += 1
            worst = max(worst, d)
    return differ, worst, wa * ha


def main():
    env = dict(os.environ, GPU_BACKEND='vulkan', GPU_STATS='1', ASAN_OPTIONS='detect_leaks=0')
    if 'VK_DRIVER_FILES' not in env and Path(LAVAPIPE).exists():
        env['VK_DRIVER_FILES'] = LAVAPIPE
    failed = 0
    print(f'{"example":36} {"GPU batches":>11} {"pixels off":>10} {"worst":>5}  result')
    for ex in sweep.load_examples():
        if ex.category != 'gpu' or ex.expected != 'runs':
            continue
        work = sweep.OUT / ex.path
        exe, soft = work / 'run', work / 'frame.bmp'
        if not exe.exists() or not soft.exists():
            print(f'{ex.path:36} not built: run make compat first')
            failed += 1
            continue
        options = list(ex.options)
        dsx = sorted(work.glob('*.3dsx'))
        if (work / 'romfs').is_dir() and dsx and '--romfs' not in options:
            options += ['--romfs', dsx[0].name]
        frame = work / 'frame_vk.bmp'
        frame.unlink(missing_ok=True)
        p = subprocess.run(['./run', *options, '--dump', frame.name], cwd=work, env=env,
                           capture_output=True, text=True, errors='replace', timeout=600)
        (work / 'run_vk.log').write_text(p.stdout + p.stderr)
        batches = sum(int(n) for n in re.findall(r'(\d+) batches drawn with Vulkan', p.stderr))
        if p.returncode != 0 or not frame.exists():
            print(f'{ex.path:36} {batches:11} exit {p.returncode}, see {work / "run_vk.log"}')
            failed += 1
            continue
        r = compare(soft, frame)
        if r is None:
            print(f'{ex.path:36} {batches:11} frames of different sizes')
            failed += 1
            continue
        differ, worst, total = r
        ok = worst <= TOLERANCE or differ <= MAX_EDGE_PIXELS * total
        failed += not ok
        print(f'{ex.path:36} {batches:11} {differ:10} {worst:5}  {"ok" if ok else "DIFFERENT"}')
    print('PASS' if not failed else f'FAIL: {failed} example(s)')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
