@ VFP edge cases, checked instruction by instruction against Unicorn (the
@ M3 driver gives r0-r3 and the VFP registers random values: NaNs,
@ infinities, denormals, signed zeros and random bit patterns).

    .syntax unified
    .arm
    .fpu vfp
    .text

    .macro FUNC name
    .global \name
    .type \name, %function
\name:
    .endm

    .macro END name
    bx lr
    .size \name, . - \name
    .endm

@ ---- arithmetic: ARM NaN rules (which NaN wins, default NaN 0x7fc00000) ----
FUNC f32_ops
    vadd.f32 s4, s0, s1
    vsub.f32 s5, s1, s0
    vmul.f32 s6, s0, s2
    vdiv.f32 s7, s2, s1
    vsqrt.f32 s8, s3
    vneg.f32 s9, s2
    vabs.f32 s10, s3
END f32_ops
FUNC f64_ops
    vadd.f64 d4, d0, d1
    vsub.f64 d5, d1, d0
    vmul.f64 d6, d0, d2
    vdiv.f64 d7, d2, d1
    vsqrt.f64 d3, d3
    vneg.f64 d0, d2
    vabs.f64 d1, d1
END f64_ops
FUNC mac_f32                    @ multiply-accumulate family (not fused)
    vmla.f32 s4, s0, s1
    vmls.f32 s5, s1, s2
    vnmla.f32 s6, s2, s3
    vnmls.f32 s7, s3, s0
    vnmul.f32 s8, s0, s3
END mac_f32
FUNC mac_f64
    vmla.f64 d4, d0, d1
    vmls.f64 d5, d1, d2
    vnmla.f64 d6, d2, d3
    vnmls.f64 d7, d3, d0
END mac_f64

@ ---- compares: unordered (NaN) sets C and V ----
FUNC cmp_f32
    vcmp.f32 s0, s1
    vmrs APSR_nzcv, fpscr
    movgt r0, #1
    movlt r1, #1
    moveq r2, #1
    movvs r3, #1
END cmp_f32
FUNC cmpe_f64_zero
    vcmpe.f64 d1, #0
    vmrs APSR_nzcv, fpscr
    movmi r0, #7
    movcs r1, #9
END cmpe_f64_zero

@ ---- conversions: float->int truncates and saturates, NaN -> 0 ----
FUNC cvt_f2i
    vcvt.s32.f32 s4, s0
    vcvt.u32.f32 s5, s1
    vcvt.s32.f64 s6, d1
    vcvt.u32.f64 s7, d2
    vmov r0, s4
    vmov r1, s5
    vmov r2, s6
    vmov r3, s7
END cvt_f2i
FUNC cvt_i2f
    vmov s4, r0
    vmov s5, r1
    vcvt.f32.s32 s6, s4
    vcvt.f32.u32 s7, s5
    vcvt.f64.s32 d4, s4
    vcvt.f64.u32 d5, s5
END cvt_i2f
FUNC cvt_ff
    vcvt.f64.f32 d4, s0         @ NaN payloads are kept, signalling NaNs quieted
    vcvt.f32.f64 s1, d1
    vcvt.f32.f64 s2, d2
END cvt_ff

@ ---- moves and memory ----
FUNC moves
    vmov r2, r3, d1             @ d1 -> two core registers
    vmov d2, r0, r1             @ two core registers -> d2
    vmov.f32 s0, s3
    vmov.f64 d3, d0
    vmov r0, s2
END moves
FUNC mem_ops                    @ r0 = pointer
    vstr s0, [r0, #8]
    vstr d1, [r0, #16]
    vldr s4, [r0, #4]
    vldr d3, [r0, #24]
    vstmia r0!, {s1-s3}
    vldmdb r0!, {d4-d5}
    vpush {d8-d9}
    vmov.f64 d8, d1
    vpop {d8-d9}
END mem_ops
FUNC literals
    vldr s0, 1f
    vldr d1, 2f
    bx lr
    .p2align 3
1:  .float 1.5
    .word 0
2:  .double -3.25
    .size literals, . - literals

@ ---- switching to libctru's RunFast mode (flush-to-zero + default NaN) ----
FUNC runfast
    mov r0, #0x03000000
    vmsr fpscr, r0
    vadd.f32 s4, s0, s1
    vmul.f64 d3, d1, d2
    vcvt.f32.f64 s5, d2
    vcmp.f32 s0, s1
    vmrs APSR_nzcv, fpscr
    movvs r1, #1
END runfast

@ ---- conditional VFP instruction ----
FUNC cond_vmov
    cmp r0, r1
    vmovgt.f32 s0, s1
    vmovle.f64 d2, d3
END cond_vmov
