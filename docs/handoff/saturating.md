# Brief: the saturating arithmetic instructions (qadd, qsub, qdadd, qdsub)

Read `CLAUDE.md` first. This job needs no game: only the public toolchain
and our own tests. Don't push; report back with a diff.

## Goal

`make fuzz` found that the translator rejects the ARMv5TE/ARMv6 saturating
instructions. Translate them, with the sticky Q flag they set, and prove
each against Unicorn.

## The instructions (ARM mode only; Thumb-1 has none)

- `qadd rd, rm, rn`: rd = sat32(rm + rn)
- `qsub rd, rm, rn`: rd = sat32(rm - rn)
- `qdadd rd, rm, rn`: rd = sat32(rm + sat32(2 * rn))
- `qdsub rd, rm, rn`: rd = sat32(rm - sat32(2 * rn))

sat32 clamps a signed result to [-2^31, 2^31 - 1]. Note the operand order:
in the assembler syntax the *second* register (rm) is the one added to, and
the third (rn) is the one doubled. Each sets CPSR.Q (bit 27) when any
saturation happens, including the doubling. Q is sticky: only `msr` clears
it. They don't touch N, Z, C or V. All can be conditional.

Q is also set by instructions we already translate: `ssat`/`usat` when they
saturate, `smlaxy`/`smlawy` on overflow of the accumulate. Check what
`recomp/translate.py` does with Q today and make it consistent.

## What to change

1. **The context.** `runtime/context.h`'s `Ctx` has no Q flag yet. Add
   `uint32_t q;` (0/1, like n/z/c/v). Find every place that builds or reads
   the CPSR (`mrs`, `msr`, the M3 driver's comparisons, anything that saves
   flags) and include bit 27. Keep it minimal.
2. **The translation** in `recomp/translate.py`: emit readable C, through
   small inline helpers in the runtime headers (for example
   `sat_add32(ctx, a, b)` setting `ctx->q`). Every instruction keeps its
   address and disassembly comment, like the others.
3. **The test**: extend `tests/real/dsp.S` (or add `tests/real/sat.S` with
   a Makefile rule like its neighbours) with every instruction against
   edge values: 0, 1, -1, 0x7FFFFFFF, 0x80000000, values that overflow only
   in the doubling, conditional forms, and Q staying set across later
   instructions and being cleared by `msr`. It must run through the M3
   lockstep driver (`tests/m3/diff.c`), which should now also compare Q.
4. **The fuzzer**: remove these from its unsupported list in
   `tests/fuzz/gen.py` so they get fuzzed.

## Done when

- `make test` passes in WSL, including the new test and `make fuzz SEED=1 N=200`
  with no divergences.
- Report: the diff, what you found about Q in the existing ssat/usat/smla
  translations, and anything in Unicorn's behaviour that looked wrong.
