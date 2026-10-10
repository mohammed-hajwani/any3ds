@ Negative test: a bl into a data section. Calls into the middle of a
@ function, or into assembler-marked code with no symbol, are fine (the
@ recompiler makes them entry points), but data is not code: this must be
@ a hard error.

    .syntax unified
    .arm
    .text

    .global caller
    .type caller, %function
caller:
    push {r4, lr}
    bl not_code
    pop {r4, pc}
    .size caller, . - caller

    .data
not_code:                       @ a word in .data, not code
    .word 0xe12fff1e
