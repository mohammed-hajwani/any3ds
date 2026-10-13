@ ARMv5TE "DSP" instructions and unusual control flow, checked against
@ Unicorn:
@   smulxy / smlaxy   16 x 16 multiply (B = bottom half, T = top half,
@                     both signed), optionally plus an accumulator
@   smulwy / smlawy   32 x 16 multiply keeping bits 47..16, optionally plus
@                     an accumulator
@   mov pc, lr        the pre-interworking return (newlib's camera code)
@   fall-through      a function that runs straight on into the next one
@                     (libgcc's __aeabi_dsub does this into __adddf3)
@   uqsub8, pld, ldr pc, [sp], #N (see the end of the file)
@ The accumulating forms set the sticky Q flag on signed overflow; we don't
@ model Q, and nothing we recompile reads it (mrs is a hard error).

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

    .macro MUL3 op
FUNC \op\()_
    \op r0, r1, r2
END \op\()_
    .endm

    .macro MUL4 op
FUNC \op\()_
    \op r0, r1, r2, r3
END \op\()_
    .endm

    MUL3 smulbb
    MUL3 smulbt
    MUL3 smultb
    MUL3 smultt
    MUL3 smulwb
    MUL3 smulwt
    MUL4 smlabb
    MUL4 smlabt
    MUL4 smlatb
    MUL4 smlatt
    MUL4 smlawb
    MUL4 smlawt

@ rd is also a source.
FUNC smlabb_self
    smlabb r1, r1, r1, r1
    mov r0, r1
END smlabb_self

FUNC mov_pc_lr
    add r0, r0, r1
    cmp r0, #100
    movhi pc, lr                @ conditional return
    add r0, r0, #7
    mov pc, lr
    .size mov_pc_lr, . - mov_pc_lr

@ negate_then_add flips r1's sign and runs on into add_two, like
@ __aeabi_dsub into __adddf3.
FUNC negate_then_add
    rsb r1, r1, #0
    .size negate_then_add, . - negate_then_add
FUNC add_two
    add r0, r0, r1
END add_two

@ Saturation: usat clamps a signed value to 0 .. 2^n - 1, ssat to
@ -2^(n-1) .. 2^(n-1) - 1, after an optional lsl / asr of the input.
@ (Both set the sticky Q flag when they clamp; not modelled, see above.)
FUNC usat_8
    usat r0, #8, r1
END usat_8
FUNC usat_0
    usat r0, #0, r1
END usat_0
FUNC usat_31_asr
    usat r0, #31, r1, asr #3
END usat_31_asr
FUNC ssat_16
    ssat r0, #16, r1
END ssat_16
FUNC ssat_1_lsl
    ssat r0, #1, r1, lsl #4
END ssat_1_lsl
FUNC ssat_32
    ssat r0, #32, r1
END ssat_32

@ ldm loading pc with other bases and modes: old APCS frame epilogues.
FUNC apcs_frame
    mov ip, sp
    push {fp, ip, lr, pc}       @ the APCS frame: stores pc too
    sub fp, ip, #4
    add r0, r0, r1
    ldmdb fp, {fp, sp, pc}      @ restore fp and sp, return through the saved lr
    .size apcs_frame, . - apcs_frame

FUNC ldm_sp_sp_lr_pc
    mov r3, sp
    sub sp, sp, #12
    str r3, [sp]                @ saved sp
    str lr, [sp, #4]            @ saved lr
    str lr, [sp, #8]            @ where to return
    add r0, r0, #1
    ldm sp, {sp, lr, pc}        @ base in the list, no writeback: the loaded sp wins
    .size ldm_sp_sp_lr_pc, . - ldm_sp_sp_lr_pc

@ `add pc, pc, rX, lsl #2` with a negative index jumps backwards (newlib's
@ _vfprintf_r loads signed offsets with ldrsh). pc reads as A + 8 here,
@ so index -4 lands on A - 8 and -3 on A - 4.
FUNC backward_jump
    and r1, r1, #1
    sub r1, r1, #4              @ index -4 or -3
    b 2f
    add r0, r0, #10             @ A - 8 (index -4), then on into...
    b 3f                        @ A - 4 (index -3)
2:  add pc, pc, r1, lsl #2      @ A
    nop
3:  add r0, r0, #1
    bx lr
    .size backward_jump, . - backward_jump

@ Coprocessors the 3DS doesn't have (here iWMMXt, p1): libgcc's unwinder
@ has save/restore helpers for them that never run on a 3DS. On an ARM11
@ executing one raises an Undefined Instruction exception, so it must
@ recompile to a trap; this one is never reached.
FUNC absent_coprocessor
    cmp r0, r0
    bxeq lr                     @ always taken
    ldcl p1, c0, [r0], #8
    stcl p1, c0, [r0], #8
    ldc2 p1, c8, [r0], #4
    mrrc p1, 0, r2, r3, c0
    .fpu vfpv3                  @ (assembler only: the 3DS's VFPv2 has no d16-d31)
    vldmia r0, {d16-d17}
    bx lr
    .size absent_coprocessor, . - absent_coprocessor

@ Found in a commercial (ARMCC-built) program:
@   uqsub8            each byte a - b, unsigned, saturating at 0
@   pkhbt / pkhtb     pack a halfword from each of two registers
@   smuad/smusd/smlad/smlsd (+x)   dual signed 16 x 16 multiplies
@   uxtb16 / sxtb16   bytes 0 and 2 into the two halfwords
@   ldrexd / strexd   64-bit exclusive access
@   pld               a cache preload hint: no architectural effect
@   ldr pc, [sp], #N  a return that also drops N - 4 more bytes of stack
    MUL3 uqsub8

FUNC preload
    pld [r1, #0x40]
    add r0, r1, #1
END preload

FUNC pop_pc_and_drop
    sub sp, sp, #16             @ 16 bytes the return then drops
    push {lr}
    add r0, r1, r2
    ldr pc, [sp], #0x14
    .size pop_pc_and_drop, . - pop_pc_and_drop

FUNC pkhbt_0
    pkhbt r0, r1, r2
END pkhbt_0

FUNC pkhbt_16
    pkhbt r0, r1, r2, lsl #16
END pkhbt_16

FUNC pkhtb_16
    pkhtb r0, r1, r2, asr #16
END pkhtb_16

FUNC pkhtb_32
    pkhtb r0, r1, r2, asr #32
END pkhtb_32

    MUL3 smuad
    MUL3 smuadx
    MUL3 smusd
    MUL3 smusdx
    MUL4 smlad
    MUL4 smladx
    MUL4 smlsd
    MUL4 smlsdx

FUNC uxtb16_ror8
    uxtb16 r0, r1, ror #8
END uxtb16_ror8

FUNC sxtb16_
    sxtb16 r0, r1
END sxtb16_

@ r0 = pointer to 8 bytes: read them exclusively, add r2 to the low word,
@ store back exclusively; r0 = status (0), r1 = the old low word.
FUNC exclusive64
    push {r4, r5}
    mov r12, r0
    ldrexd r4, r5, [r12]
    mov r1, r4
    add r4, r4, r2
    strexd r0, r4, r5, [r12]
    pop {r4, r5}
END exclusive64
