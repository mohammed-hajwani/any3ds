# CPU instruction fuzzer

From WSL with the normal devkitPro environment:

```
make fuzz SEED=1 N=200
```

N is the number of functions **per mode**, ARM and Thumb. Each function has
10–40 generated instructions, followed by a return. ARM also has a two-instruction
GE reset: the existing M3 driver otherwise preserves Unicorn's GE bits between
runs while zeroing ours. This avoids false SEL divergences without changing M3.
The M3 driver supplies edge-case integer and VFP inputs and both ordinary and
RunFast FPSCR modes. Ten input runs are used per function.

`gen.py` imports only Python's standard library. It invokes the assembler,
recompiler, host compiler and existing `tests/m3/diff.c`; it does not implement a
second CPU oracle. Compiler overrides from the Makefile are passed through.

Generated assembly, disassembly, compiler logs, per-function results, a summary
and the separate unsupported-instruction list go under `out/fuzz/SEED/`. A rejected
instruction removes its address-setup unit, then the remaining program is rebuilt
and tested. Rejections are gaps, not successful comparisons. An assembler error,
timeout or non-divergence harness failure exits with status 2 rather than claiming
a pass. Confirmed mismatches exit 1; a clean comparison exits 0.

Mismatch reproducers go in `tests/fuzz/found/`, with seed, mode, input-run count
and original encodings. The shrinker deletes chunks and then individual units,
preserving the failing mnemonic. Results are deletion-minimal **units**, not a
proof of a globally shortest program; memory setup and balanced push/pop stay
together. Inspect the minimized log under `out/fuzz/SEED/` before deciding which
side is wrong. The translator is never modified by the fuzzer.

Memory bases stay inside M3's 4 KiB scratch region; random arithmetic cannot
overwrite them. No random PC/SP destinations, SVCs, external branches, or
privileged instructions are generated. Thumb covers data processing, shifts,
high registers, PC/SP-relative reads, extensions, memory and balanced stack/multiple
transfers; branch/exception formats are deliberately excluded by this safety policy.
Q and GE are not directly compared by M3; SEL exercises GE indirectly.

Generate without compiling or running:

```
.venv/bin/python tests/fuzz/gen.py --seed 1 -n 200 --generate-only --output out/fuzz-preview
```

Long fuzz runs are intentionally separate from `make test`. Found cases need
human triage, and runs longer than 30 seconds do not belong in the quick suite.
