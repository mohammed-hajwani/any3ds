@ Negative test: a 32-bit Thumb-2 instruction. ARMv6K (the 3DS CPU) only
@ has Thumb-1 plus bl/blx, so this must be rejected, not translated.
    .syntax unified
    .thumb
    .text
    .global wide
    .type wide, %function
    .thumb_func
wide:
    .inst.w 0xf3af8000          @ nop.w
    bx lr
    .size wide, . - wide
