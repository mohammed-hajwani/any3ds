@ Thumb-1 edge cases, checked instruction by instruction against Unicorn
@ by the M3 driver (r0-r3 get random inputs, often pointers to scratch).

    .syntax unified
    .thumb
    .text

    .macro FUNC name
    .p2align 2
    .global \name
    .type \name, %function
    .thumb_func
\name:
    .endm

    .macro END name
    bx lr
    .size \name, . - \name
    .endm

@ ---- literal loads: pc reads as this instruction + 4, aligned down to 4 ----
FUNC lit_aligned                @ ldr at an address that is 0 mod 4
    ldr r1, =0x11223344
    bx lr
    .ltorg
    .size lit_aligned, . - lit_aligned

FUNC lit_unaligned              @ ldr at an address that is 2 mod 4
    nop
    ldr r1, =0x55667788
    bx lr
    .ltorg
    .size lit_unaligned, . - lit_unaligned

FUNC adr_data                   @ add r1, pc, #n  (adr), then load through it
    adr r1, 1f
    ldr r2, [r1]
    ldr r3, [r1, #4]
    bx lr
    .p2align 2
1:  .word 0xA5A5F00D, 0x0BADBEEF
    .size adr_data, . - adr_data

@ ---- register-specified shifts: only the bottom byte of the amount counts,
@      and 0, 32 and >32 each have their own result and carry ----
    .macro SHIFT op, name, amount
FUNC \name
    movs r1, #\amount
    \op r0, r1
END \name
    .endm

    .macro SHIFT256 op, name
FUNC \name
    movs r1, #1
    lsls r1, r1, #8             @ 256: bottom byte is 0 -> no shift at all
    \op r0, r1
END \name
    .endm

    .macro ALLSHIFTS op
    SHIFT \op, \op\()_0, 0
    SHIFT \op, \op\()_1, 1
    SHIFT \op, \op\()_31, 31
    SHIFT \op, \op\()_32, 32
    SHIFT \op, \op\()_33, 33
    SHIFT \op, \op\()_255, 255
    SHIFT256 \op, \op\()_256
FUNC \op\()_rnd                 @ amount = whatever random r1 holds
    \op r0, r1
END \op\()_rnd
    .endm

    ALLSHIFTS lsls
    ALLSHIFTS lsrs
    ALLSHIFTS asrs
    ALLSHIFTS rors

@ ---- other flag-setting ALU ops ----
FUNC negs_                      @ rsbs r0, r1, #0
    negs r0, r1
END negs_
FUNC muls_                      @ N,Z from the result; C,V unchanged
    muls r0, r1
END muls_
FUNC adcs_sbcs
    adcs r0, r1
    sbcs r2, r3
END adcs_sbcs
FUNC bics_mvns
    bics r0, r1
    mvns r2, r3
END bics_mvns

@ ---- high registers: add/mov don't touch flags, cmp does ----
FUNC hireg
    mov r8, r1
    add r0, r8
    mov r9, r0
    cmp r9, r2
    mov r1, r9
    add r8, r3
    mov r3, r8
END hireg

@ ---- sp-relative and block transfers ----
FUNC sp_rel
    sub sp, #16
    str r1, [sp, #4]
    str r2, [sp, #12]
    ldr r0, [sp, #4]
    ldr r3, [sp, #12]
    add r2, sp, #8
    add sp, #16
END sp_rel
FUNC ldm_stm                    @ r0 = base pointer
    movs r2, #7
    stmia r0!, {r1, r2}
    subs r0, #8
    ldmia r0!, {r2, r3}
END ldm_stm
FUNC byte_half
    ldrb r2, [r0, #3]
    ldrh r3, [r0, #6]
    strb r2, [r0, #1]
    strh r3, [r0, #10]
    movs r1, #5
    ldrsb r2, [r0, r1]
    ldrsh r3, [r0, r1]
END byte_half

@ ---- control flow ----
FUNC loop_bcc                   @ count down with a conditional branch
    movs r2, #0
1:  adds r2, #3
    subs r1, #1
    bhi 1b
    movs r0, r2
END loop_bcc
FUNC push_pop_call              @ bl within Thumb, saved lr
    push {r4, lr}
    movs r4, r1
    bl negs_
    adds r0, r4
    pop {r4, pc}
    .size push_pop_call, . - push_pop_call
