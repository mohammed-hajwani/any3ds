"""Regenerate a program in trace mode with a deliberately planted translator
bug. Used by selftest.sh to prove the differential driver catches *and
pinpoints* real bugs.

Usage: python tests/m3/plant_bug.py <bug> <input.elf> <output.c>
Bugs:
  ldrsh     ldrsh loses its sign extension
  thumb-pc  pc reads as +8 in Thumb too (it should be +4)
  vfp-sqrt  vsqrt.f32 as plain C sqrtf: x86's default NaN (0xffc00000)
            instead of ARM's (0x7fc00000) for negative inputs
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from capstone.arm import ARM_INS_LDRSH  # noqa: E402

import recomp.translate as translate  # noqa: E402
from recomp.__main__ import main  # noqa: E402

bug, elf, out = sys.argv[1:4]
if bug == 'ldrsh':
    translate._LOADS[ARM_INS_LDRSH] = ('read16', None, 2)
elif bug == 'thumb-pc':
    translate._pc = lambda insn: insn.address + 8
elif bug == 'vfp-sqrt':
    from capstone.arm import ARM_INS_VSQRT
    original = translate._HANDLERS[ARM_INS_VSQRT]

    def plain_sqrt(insn, fc):
        (k, d), (_, m) = translate._vfp_regs(insn, 2)
        if k != 's':
            return original(insn, fc)
        return [f'ctx->vfp.u[{d}] = f32_bits(__builtin_sqrtf(f32(ctx->vfp.u[{m}])));']
    translate._HANDLERS[ARM_INS_VSQRT] = plain_sqrt
else:
    sys.exit(f'unknown bug {bug!r}')
sys.exit(main([elf, '-o', out, '--trace']))
