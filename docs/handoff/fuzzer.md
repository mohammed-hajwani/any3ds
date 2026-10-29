# Brief: a random-instruction fuzzer for the translator

Read `CLAUDE.md` first. This job needs no game: it only uses the public
toolchain and our own tests. Don't push; report back with a diff.

## Goal

Find translation bugs before a game does. Generate random ARMv6K
instruction sequences, recompile them, and run them in lockstep against
Unicorn with the existing M3 driver. Every mismatch becomes a small `.S`
reproducer.

## What already exists (reuse it, don't rewrite it)

- `tests/m3/diff.c`: runs every function of a linked test program in
  lockstep with Unicorn 2.0.1, comparing r0-r14, NZCV, d0-d15, FPSCR flags
  and memory writes before every instruction. It names the first
  divergence.
- `tests/real/*.S`, `tests/m5/vfp.S`: hand-written test programs that go
  through it. Their Makefile rules show how a `.S` is assembled, linked at
  `0x00100000`, recompiled with `python -m recomp --trace`, and diffed.
- `tests/m3/selftest.sh`: checks that a planted bug is pinpointed.

## What to build

`tests/fuzz/gen.py` (Python standard library only) plus a `make fuzz` rule:

1. **Generator.** With a seed, write a `.S` file of N functions, each a
   straight-line block of 10-40 random instructions that ends in `bx lr`.
   Cover:
   - ARM: data processing (every opcode, S bit, all immediate and shifted
     register forms, including shift by register), mul/mla, long
     multiplies, clz, the ARMv6 media instructions (parallel add/sub,
     saturating, extend, rev, usat/ssat, smul*/smla*), conditional
     execution on everything.
   - Loads/stores in every addressing mode, ldm/stm, ldrd/strd, ldrex/strex.
     Base registers must point into a scratch buffer: set them up at the
     start of each function, and keep offsets in range.
   - VFPv2 (arm only, hard-float): arithmetic, compares + `vmrs APSR_nzcv`,
     conversions, loads/stores; include NaNs, infinities, denormals, -0,
     and runs with RunFast mode (FPSCR FZ+DN) on.
   - Thumb-1 (a second file, `.thumb`): every 16-bit format.
   - Exclude what we can't diff: writes to pc or sp other than balanced
     push/pop, svc, branches out of the block, coprocessor ops, mrs/msr
     beyond the flags.
2. **Runner.** `make fuzz SEED=1 N=200` generates, builds, recompiles and
   diffs. It must be deterministic for a given seed.
3. **Triage.** For each divergence, shrink it to the fewest instructions
   that still diverge, and write `tests/fuzz/found/<mnemonic>-<seed>.S`
   with the original encoding in a comment.
4. **Unsupported instructions.** The recompiler hard-errors on anything it
   doesn't translate, naming the address and mnemonic. The fuzzer must
   collect those into a separate list (they are gaps, not bugs) and carry
   on without them.
5. Add a short fixed-seed run to `make test` only if it takes under 30 s.

## Done when

- `make fuzz SEED=1 N=200` runs end to end in WSL, and `make test` still
  passes.
- Report: how many instructions were covered, every divergence found (with
  its reproducer and your reading of which side is wrong: ours or
  Unicorn's; Unicorn has bugs too), and the list of unsupported
  instructions. Don't fix the translator yet: the owner and Claude decide
  each fix.
