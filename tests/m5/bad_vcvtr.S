@ Negative test: vcvtr rounds using the FPSCR rounding mode, which the
@ recompiler doesn't model (it assumes round-to-nearest everywhere).
@ Refuse it rather than silently round the wrong way.
    .syntax unified
    .arm
    .fpu vfp
    .text
    .global round_by_mode
    .type round_by_mode, %function
round_by_mode:
    vcvtr.s32.f32 s0, s1
    bx lr
    .size round_by_mode, . - round_by_mode
