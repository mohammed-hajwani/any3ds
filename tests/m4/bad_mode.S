@ Negative test: ARM code inside a function whose symbol says Thumb.
@ Mixing modes inside one function isn't supported; refuse it.
    .syntax unified
    .text
    .global mixed
    .type mixed, %function
    .thumb
    .thumb_func
mixed:
    movs r0, #1
    .p2align 2
    .arm
    mov r0, #2
    bx lr
    .size mixed, . - mixed
