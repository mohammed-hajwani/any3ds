@ Computed jumps as hand-written assembly does them (Pokémon X's video
@ decoder), checked against Unicorn. Recompiled with --no-symbols: function
@ discovery starts at _start, reads each table, and the translator turns
@ each jump into a switch over the targets it found.
@   add pc, rT, rO    rT = a table's address (sub rT, pc, #n), rO = an
@                     offset loaded from the table (index scaled by 4);
@                     the word after the table is not an entry
@   mov pc, rX        after add rX, rT, rO, with other instructions between
@   add pc, rT, rO    offsets back to before the function, indexed by a
@                     byte offset: those targets are functions of their own
@   mov pc, rX        returning through a copy of lr
@   add pc, rT, rO    after `adr lr, label`: an indexed call that returns
@                     into the loop around it
@   bne / b / bx rX   after `adr lr, label`: calls too (conditional ones
@                     as well), returning to the label
@   movlt pc, rX      to a fixed address built from pc in two steps
@   b label           after such calls return: a plain jump again

    .syntax unified
    .arm
    .text
    .global _start
_start:
    push {r4, lr}
    bl add_table
    bl mov_table
    bl negative_table
    bl return_copy
    bl call_loop
    bl branch_calls
    bl register_call
    bl const_jump
    bl cont_then_jump
    pop {r4, pc}

add_table:
    and r1, r1, #3
    b 2f
1:  .word 10f - 1b, 11f - 1b, 12f - 1b, 13f - 1b
    .word 0x05040301            @ (a byte table after it: not an entry)
2:  adr r2, 1b                  @ sub r2, pc, #n
    ldr r3, [r2, r1, lsl #2]
    add pc, r3, r2
10: add r0, r0, #1
    bx lr
11: add r0, r0, #2
    bx lr
12: sub r0, r0, #3
    bx lr
13: eor r0, r0, #4
    bx lr

mov_table:
    and r1, r1, #1
    adr ip, 3f                  @ add ip, pc, #n
    ldr r3, [ip, r1, lsl #2]
    add ip, ip, r3
    mov r2, #5                  @ unrelated work before the jump
    mov pc, ip
3:  .word 20f - 3b, 21f - 3b
20: mul r0, r2, r0
    bx lr
21: add r0, r0, r2
    bx lr

30: add r0, r0, #100
    bx lr
31: sub r0, r0, #100
    bx lr
negative_table:
    and r1, r1, #1
    lsl r1, r1, #2              @ a byte offset into the table
    adr r2, 4f
    ldr r3, [r2, r1]
    add pc, r2, r3
4:  .word 30b - 4b, 31b - 4b

return_copy:
    mov r12, lr
    bl helper
    mov pc, r12                 @ back to our caller
helper:
    add r0, r0, r1
    bx lr

call_loop:
    push {r4, lr}
    and r4, r1, #7              @ that many calls
5:  subs r4, r4, #1
    blt 6f
    and r1, r4, #1
    adr lr, 5b                  @ where the call returns: back into the loop
    adr r2, 7f
    ldr r3, [r2, r1, lsl #2]
    add pc, r2, r3
6:  pop {r4, pc}
7:  .word 40f - 7b, 41f - 7b
40: add r0, r0, #3
    bx lr
41: eor r0, r0, #0x55
    bx lr

branch_calls:
    push {r4, lr}
    tst r1, #1
    adr lr, 8f                  @ both calls below return to 8
    bne 50f                     @ a call when r1 is odd
    b 51f                       @ else this one
8:  add r0, r0, #7
    pop {r4, pc}
50: add r0, r0, #1000
    bx lr
51: sub r0, r0, #1000
    bx lr

register_call:
    push {r4, lr}
    adr r3, 52f
    adr lr, 9f
    bx r3                       @ a call through a register
9:  add r0, r0, #5
    pop {r4, pc}
52: eor r0, r0, #0xff
    bx lr

const_jump:
60: add r3, pc, #0              @ r3 = 60b + 8
    add r3, r3, #(53f - 60b - 8)
    cmp r1, #0
    movlt pc, r3                @ a conditional jump to a fixed address
    add r0, r0, #9
    bx lr
53: rsb r0, r0, #0
    bx lr

cont_then_jump:
    push {r4, lr}
    tst r1, #1
    adr lr, 70f                 @ both calls return to 70
    beq 71f
    bne 72f
70: add r0, r0, #1
    b 73f                       @ a plain jump after the calls, not another call
71: add r0, r0, #10
    bx lr
72: add r0, r0, #20
    bx lr
73: pop {r4, pc}
