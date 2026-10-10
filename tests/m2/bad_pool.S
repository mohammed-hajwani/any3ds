@ Negative test: code that runs straight into its literal pool. On real
@ hardware the CPU would execute the data word; the recompiler must refuse.
    .syntax unified
    .arm
    .text
    .global falls_into_pool
    .type falls_into_pool, %function
falls_into_pool:
    ldr r0, =0x12345678
    add r0, r0, #1              @ no return here
    .ltorg
    .size falls_into_pool, . - falls_into_pool
