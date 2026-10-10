@ Negative test: loading pc from an arbitrary address is an indirect jump.
@ Not supported yet, so it must be a hard error.
    .syntax unified
    .arm
    .text
    .global jump_via_memory
    .type jump_via_memory, %function
jump_via_memory:
    ldr pc, [r0]
    .size jump_via_memory, . - jump_via_memory
