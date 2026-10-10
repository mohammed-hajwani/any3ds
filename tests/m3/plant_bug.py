"""Regenerate a program in trace mode with a deliberately planted translator
bug: ldrsh loses its sign extension. Used by selftest.sh to prove the
differential driver catches *and pinpoints* real bugs.

Usage: python tests/m3/plant_bug.py <input.elf> <output.c>
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from capstone.arm import ARM_INS_LDRSH  # noqa: E402

import recomp.translate as translate  # noqa: E402
from recomp.__main__ import main  # noqa: E402

translate._LOADS[ARM_INS_LDRSH] = ('read16', None, 2)      # the planted bug
sys.exit(main([sys.argv[1], '-o', sys.argv[2], '--trace']))
