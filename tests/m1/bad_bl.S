@ Negative test: a bl to code that belongs to no function symbol at all.
@ (A bl into the *middle* of a known function is fine: the recompiler
@ makes that target an entry point of its own. But code outside every
@ function is unknown territory, so this must be a hard error.)

    .syntax unified
    .arm
    .text

    .global caller
    .type caller, %function
caller:
    push {r4, lr}
    bl orphan
    pop {r4, pc}
    .size caller, . - caller

orphan:                         @ plain label, not inside any function
    mov r0, #2
    bx lr
