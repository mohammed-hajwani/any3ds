@ Barrel-shifter coverage for M0.
@
@ Every ARM data-processing instruction can shift its last register operand
@ for free ("operand 2"). Each function here computes  r0 = r0 + shift(r1)
@ so the harness can check one shift type and amount at a time.
@
@ Written in assembly because a C compiler rarely emits the edge cases:
@   lsr #32 / asr #32  are encoded with a shift amount of 0
@   rrx                is the encoding "ror #0", and shifts the carry flag in

    .syntax unified
    .arm
    .text

    .global add_reg
    .type add_reg, %function
add_reg:
    add r0, r0, r1
    bx lr
    .size add_reg, . - add_reg

    .global add_imm
    .type add_imm, %function
add_imm:
    add r0, r0, #0xff000000     @ 0xff rotated right by 8: an "ARM immediate"
    bx lr
    .size add_imm, . - add_imm

    .global lsl_1
    .type lsl_1, %function
lsl_1:
    add r0, r0, r1, lsl #1
    bx lr
    .size lsl_1, . - lsl_1

    .global lsl_31
    .type lsl_31, %function
lsl_31:
    add r0, r0, r1, lsl #31
    bx lr
    .size lsl_31, . - lsl_31

    .global lsr_1
    .type lsr_1, %function
lsr_1:
    add r0, r0, r1, lsr #1
    bx lr
    .size lsr_1, . - lsr_1

    .global lsr_32
    .type lsr_32, %function
lsr_32:
    add r0, r0, r1, lsr #32
    bx lr
    .size lsr_32, . - lsr_32

    .global asr_1
    .type asr_1, %function
asr_1:
    add r0, r0, r1, asr #1
    bx lr
    .size asr_1, . - asr_1

    .global asr_31
    .type asr_31, %function
asr_31:
    add r0, r0, r1, asr #31
    bx lr
    .size asr_31, . - asr_31

    .global asr_32
    .type asr_32, %function
asr_32:
    add r0, r0, r1, asr #32
    bx lr
    .size asr_32, . - asr_32

    .global ror_1
    .type ror_1, %function
ror_1:
    add r0, r0, r1, ror #1
    bx lr
    .size ror_1, . - ror_1

    .global ror_31
    .type ror_31, %function
ror_31:
    add r0, r0, r1, ror #31
    bx lr
    .size ror_31, . - ror_31

    .global rrx_
    .type rrx_, %function
rrx_:
    add r0, r0, r1, rrx
    bx lr
    .size rrx_, . - rrx_
