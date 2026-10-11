@ ARMv6 extends, checked against Unicorn: uxtb/uxth/sxtb/sxth and the
@ extend-and-add forms uxtab/uxtah/sxtab/sxtah (rd = rn + extend(rm)),
@ each with every rotation (ror #0/8/16/24 picks which byte or halfword).
@ newlib's string code uses uxtah; libctru's locks use uxtab.

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

    .macro EXT op, name
FUNC \name\()_0
    \op r0, r1
END \name\()_0
FUNC \name\()_8
    \op r0, r1, ror #8
END \name\()_8
FUNC \name\()_16
    \op r0, r1, ror #16
END \name\()_16
FUNC \name\()_24
    \op r0, r1, ror #24
END \name\()_24
    .endm

    .macro EXTADD op, name
FUNC \name\()_0
    \op r0, r2, r1
END \name\()_0
FUNC \name\()_8
    \op r0, r2, r1, ror #8
END \name\()_8
FUNC \name\()_16
    \op r0, r2, r1, ror #16
END \name\()_16
FUNC \name\()_24
    \op r0, r2, r1, ror #24
END \name\()_24
    .endm

    EXT uxtb, uxtb
    EXT uxth, uxth
    EXT sxtb, sxtb
    EXT sxth, sxth
    EXTADD uxtab, uxtab
    EXTADD uxtah, uxtah
    EXTADD sxtab, sxtab
    EXTADD sxtah, sxtah

@ rd may also be one of the sources.
FUNC uxtah_self
    uxtah r1, r1, r1, ror #16
    mov r0, r1
END uxtah_self
