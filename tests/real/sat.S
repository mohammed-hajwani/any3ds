@ Scalar saturation: operand order, intermediate saturation, sticky Q and NZCV.
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
    .irp op,qadd,qsub,qdadd,qdsub
FUNC \op\()_random
    \op r0, r1, r2
END \op\()_random
FUNC \op\()_edges
    msr CPSR_f, #0
    mov r0, #0
    mov r1, #1
    mvn r2, #0
    mvn r3, #0x80000000
    mov r4, #0x80000000
    \op r5, r0, r0
    \op r5, r1, r2
    \op r5, r3, r1
    \op r5, r4, r2
    msr CPSR_f, #0
    \op r5, r4, r3       @ QDADD: doubling overflows but outer add does not
    mrs r6, CPSR
    \op r5, r0, r0       @ Q must stay set
    mrs r7, CPSR
    msr CPSR_f, #0
    mrs r8, CPSR
    cmp r0, r0
    \op\()ne r5, r3, r3 @ condition false: no Q or register changes
    \op\()eq r5, r4, r4
END \op\()_edges
    .endr
FUNC sat_existing_q
    msr CPSR_f, #0
    mov r0, #0x80000000
    ssat r1, #8, r0
    usat r2, #8, r0
    ssat16 r3, #4, r0
    usat16 r4, #4, r0
    mvn r0, #0x80000000
    mov r1, #0x7f00
    smlabb r2, r1, r1, r0
    smlawb r3, r0, r1, r0
    mrs r4, CPSR
    msr CPSR_f, r5
    mrs r6, CPSR
END sat_existing_q
