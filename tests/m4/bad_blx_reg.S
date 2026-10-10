@ Negative test: an indirect call through a register. The target isn't
@ known at recompile time; that needs the lookup table from a later
@ milestone, so for now it must be a hard error.
    .syntax unified
    .thumb
    .text
    .global call_ptr
    .type call_ptr, %function
    .thumb_func
call_ptr:
    push {r4, lr}
    blx r0
    pop {r4, pc}
    .size call_ptr, . - call_ptr
