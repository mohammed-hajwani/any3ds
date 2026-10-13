"""How well function discovery does without symbols, measured against the
symbols it never saw.

    python tests/m11/recall.py program.elf [--min PERCENT]

Runs discovery on the ELF with its symbols ignored, then compares the
function starts it found with the ELF's function symbols. A symbol it
misses is either dead code (never called: the linker kept it, nothing uses
it) or reached only through a pointer we didn't spot; the frame tests tell
those apart, this just reports the numbers.
"""

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from recomp.discover import discover  # noqa: E402
from recomp.elf_loader import load_program  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('elf')
    ap.add_argument('--min', type=float, default=0, help='fail below this percentage of symbols found')
    args = ap.parse_args()

    truth = load_program(args.elf)
    symbols = {fn.addr: fn.name for fn in truth.functions}
    stripped = load_program(args.elf)
    stripped.functions, stripped.data_ranges, stripped.code_regions, stripped.data_symbols = [], [], [], {}
    found, report = discover(stripped)
    starts = {fn.addr for fn in found}

    hit = [a for a in symbols if a in starts]
    missed = sorted(a for a in symbols if a not in starts)
    extra = len(starts - set(symbols))
    pct = 100 * len(hit) / len(symbols)
    print(f'{Path(args.elf).name}: found {len(hit)} of {len(symbols)} function symbols ({pct:.0f}%), '
          f'plus {extra} starts with no symbol (alternate entries, code after calls that never return); '
          f'{report["pointer_functions"]} found through pointers')
    for a in missed[:15]:
        print(f'    missed {a:08x} {symbols[a]}')
    if len(missed) > 15:
        print(f'    ... and {len(missed) - 15} more')
    return 0 if pct >= args.min else 1


if __name__ == '__main__':
    sys.exit(main())
