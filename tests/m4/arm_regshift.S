@ ARM-mode register-specified shifts (operand 2 = rm, <shift> rs), checked
@ against Unicorn. Same awkward amounts as thumb.S, plus random ones.

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

    .macro SHIFT sh, name, amount
FUNC \name
    mov r2, #\amount
    movs r0, r1, \sh r2         @ value and carry from the shifter
    add r3, r3, r1, \sh r2      @ arithmetic: shifter carry ignored
END \name
    .endm

    .macro ALLSHIFTS sh
    SHIFT \sh, \sh\()_0, 0
    SHIFT \sh, \sh\()_1, 1
    SHIFT \sh, \sh\()_31, 31
    SHIFT \sh, \sh\()_32, 32
    SHIFT \sh, \sh\()_33, 33
    SHIFT \sh, \sh\()_255, 255
    SHIFT \sh, \sh\()_256, 256
FUNC \sh\()_rnd
    ands r0, r1, r0, \sh r2     @ logical with S: C from the shifter
    eor r3, r3, r1, \sh r2
END \sh\()_rnd
    .endm

    ALLSHIFTS lsl
    ALLSHIFTS lsr
    ALLSHIFTS asr
    ALLSHIFTS ror

FUNC mul_
    mul r0, r1, r2
    muls r3, r1, r0
END mul_
