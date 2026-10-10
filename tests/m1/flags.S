@ Exact flag semantics, checked by flags_harness.c against a reference model.
@
@ C code only reaches these corners by accident, so each function here runs
@ one flag-setting instruction on r0/r1 and returns. The harness inspects
@ r0 and NZCV afterwards.

    .syntax unified
    .arm
    .text

    .macro FUNC name
    .global \name
    .type \name, %function
\name:
    .endm

    .macro END name
    .size \name, . - \name
    .endm

@ ---- Arithmetic: NZCV come from the adder ----

FUNC adds_
    adds r0, r0, r1
    bx lr
END adds_

FUNC adcs_
    adcs r0, r0, r1
    bx lr
END adcs_

FUNC subs_
    subs r0, r0, r1
    bx lr
END subs_

FUNC sbcs_
    sbcs r0, r0, r1
    bx lr
END sbcs_

FUNC rsbs_                      @ r0 = r1 - r0
    rsbs r0, r0, r1
    bx lr
END rsbs_

FUNC cmp_
    cmp r0, r1
    bx lr
END cmp_

FUNC cmn_
    cmn r0, r1
    bx lr
END cmn_

FUNC adc_                       @ no S: uses C but must leave NZCV alone
    adc r0, r0, r1
    bx lr
END adc_

FUNC sbc_
    sbc r0, r0, r1
    bx lr
END sbc_

FUNC adds_lsl1                  @ arithmetic ignores the shifter's carry-out
    adds r0, r0, r1, lsl #1
    bx lr
END adds_lsl1

@ ---- Logical: N,Z from the result, C from the shifter, V untouched ----

FUNC tst_
    tst r0, r1
    bx lr
END tst_

FUNC teq_
    teq r0, r1
    bx lr
END teq_

FUNC ands_rot                   @ 0xff ror 8: a rotated immediate sets C = bit 31
    ands r0, r0, #0xff000000
    bx lr
END ands_rot

FUNC ands_small                 @ unrotated immediate: C unchanged
    ands r0, r0, #0xff
    bx lr
END ands_small

FUNC eors_lsr1
    eors r0, r0, r1, lsr #1
    bx lr
END eors_lsr1

FUNC mvns_asr1
    mvns r0, r1, asr #1
    bx lr
END mvns_asr1

FUNC movs_lsl0                  @ no shift: C unchanged
    movs r0, r1
    bx lr
END movs_lsl0

FUNC movs_lsl1
    movs r0, r1, lsl #1
    bx lr
END movs_lsl1

FUNC movs_lsl31
    movs r0, r1, lsl #31
    bx lr
END movs_lsl31

FUNC movs_lsr1
    movs r0, r1, lsr #1
    bx lr
END movs_lsr1

FUNC movs_lsr32
    movs r0, r1, lsr #32
    bx lr
END movs_lsr32

FUNC movs_asr1
    movs r0, r1, asr #1
    bx lr
END movs_asr1

FUNC movs_asr32
    movs r0, r1, asr #32
    bx lr
END movs_asr32

FUNC movs_ror1
    movs r0, r1, ror #1
    bx lr
END movs_ror1

FUNC movs_ror31
    movs r0, r1, ror #31
    bx lr
END movs_ror31

FUNC movs_rrx
    movs r0, r1, rrx
    bx lr
END movs_rrx

@ ---- Condition codes: r0 = 1 if the condition holds, else 0 ----

    .macro COND cc
FUNC c_\cc
    mov r0, #0
    mov\cc r0, #1
    bx lr
END c_\cc
    .endm

    COND eq
    COND ne
    COND cs
    COND cc
    COND mi
    COND pl
    COND vs
    COND vc
    COND hi
    COND ls
    COND ge
    COND lt
    COND gt
    COND le
