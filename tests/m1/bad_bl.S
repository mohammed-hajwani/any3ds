@ Negative test: a bl into the middle of a function.
@ The recompiler only knows how to call function entry points, so this
@ must be a hard error rather than a call to the wrong place.

    .syntax unified
    .arm
    .text

    .global caller
    .type caller, %function
caller:
    push {r4, lr}
    bl callee_mid
    pop {r4, pc}
    .size caller, . - caller

    .global callee
    .type callee, %function
callee:
    mov r0, #1
callee_mid:                     @ plain label, not a function symbol
    mov r0, #2
    bx lr
    .size callee, . - callee
