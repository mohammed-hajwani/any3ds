@ Negative test: ldm with writeback and the base register in the list is
@ UNPREDICTABLE on ARMv6. The recompiler must refuse it, not guess.
    .syntax unified
    .arm
    .text
    .global ldm_wb_base
    .type ldm_wb_base, %function
ldm_wb_base:
    .inst 0xe8b00003            @ ldm r0!, {r0, r1} (gas refuses to assemble it)
    bx lr
    .size ldm_wb_base, . - ldm_wb_base
