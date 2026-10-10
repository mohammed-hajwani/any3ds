@ Every load/store addressing mode and block-transfer variant, one per
@ function. Convention: r0 = base pointer into a scratch buffer, r1-r3 =
@ inputs. The harness checks r0-r3 and the buffer against a C model.

    .syntax unified
    .arm
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

@ ---- single loads: offset, pre-index (!) and post-index forms ----
FUNC ldr_imm
    ldr r1, [r0, #8]
END ldr_imm
FUNC ldr_neg
    ldr r1, [r0, #-4]
END ldr_neg
FUNC ldr_reg
    ldr r1, [r0, r2]
END ldr_reg
FUNC ldr_regneg
    ldr r1, [r0, -r2]
END ldr_regneg
FUNC ldr_lsl                    @ scaled index: base + r2 * 4
    ldr r1, [r0, r2, lsl #2]
END ldr_lsl
FUNC ldr_pre                    @ address = r0 + 4, then r0 = that address
    ldr r1, [r0, #4]!
END ldr_pre
FUNC ldr_post                   @ address = r0, then r0 += 4
    ldr r1, [r0], #4
END ldr_post
FUNC ldr_postreg
    ldr r1, [r0], -r2
END ldr_postreg
FUNC ldrb_
    ldrb r1, [r0, #3]
END ldrb_
FUNC ldrb_preneg
    ldrb r1, [r0, #-1]!
END ldrb_preneg
FUNC ldrh_
    ldrh r1, [r0, #2]
END ldrh_
FUNC ldrsb_                     @ sign-extends bit 7
    ldrsb r1, [r0, #5]
END ldrsb_
FUNC ldrsh_                     @ sign-extends bit 15
    ldrsh r1, [r0, #6]
END ldrsh_
FUNC ldrsh_reg
    ldrsh r1, [r0, r2]
END ldrsh_reg

@ ---- single stores ----
FUNC str_imm
    str r1, [r0, #8]
END str_imm
FUNC str_regpre
    str r1, [r0, r2]!
END str_regpre
FUNC strb_
    strb r1, [r0, #3]
END strb_
FUNC strb_post
    strb r1, [r0], #1
END strb_post
FUNC strh_
    strh r1, [r0, #2]
END strh_
FUNC strh_post
    strh r1, [r0], #2
END strh_post

@ ---- block transfers: increment/decrement, after/before ----
FUNC ldm_ia                     @ r1..r3 = words at r0, r0+4, r0+8
    ldm r0, {r1, r2, r3}
END ldm_ia
FUNC ldm_ia_wb
    ldm r0!, {r1, r2}
END ldm_ia_wb
FUNC ldmib_                     @ starts at r0+4
    ldmib r0, {r1, r2}
END ldmib_
FUNC ldmda_                     @ ends at r0
    ldmda r0, {r1, r2}
END ldmda_
FUNC ldmdb_wb                   @ ends at r0-4; r0 -= 8
    ldmdb r0!, {r1, r2}
END ldmdb_wb
FUNC ldm_base                   @ base in the list, no writeback: fine
    ldm r0, {r0, r1}
END ldm_base
FUNC stm_ia
    stm r0, {r1, r2}
END stm_ia
FUNC stmib_wb
    stmib r0!, {r1, r2}
END stmib_wb
FUNC stmda_
    stmda r0, {r1, r2}
END stmda_
FUNC stmdb_wb                   @ = push, but on r0
    stmdb r0!, {r1, r2, r3}
END stmdb_wb

@ ---- stack ----
FUNC push_pop                   @ r1 = r1 + r2 via saved r4/r5
    push {r4, r5}
    mov r4, r1
    mov r5, r2
    add r1, r4, r5
    pop {r4, r5}
END push_pop
FUNC push1_pop1                 @ single-register forms: str/ldr with writeback
    push {r1}
    pop {r2}
END push1_pop1

@ ---- literal pools: constants stored next to the code ----
FUNC pool
    ldr r1, =0xCAFEF00D
    bx lr
    .ltorg
    .size pool, . - pool

FUNC pool_data                  @ address from the pool, value from .data
    ldr r1, =datum
    ldr r1, [r1]
    bx lr
    .ltorg
    .size pool_data, . - pool_data

    .data
    .p2align 2
datum:
    .word 0x600DF00D
