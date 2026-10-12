/* The PICA200 shader units: an interpreter for the shader instruction set
 * (3dbrew: GPU/Shader_Instruction_Set).
 *
 * A program is up to 4096 32-bit instruction words plus a table of 128
 * operand descriptors. A descriptor holds, for up to three sources, a
 * swizzle (which component feeds x, y, z, w) and a negate bit, plus the
 * destination's write mask. Registers: v0-v15 inputs, o0-o15 outputs,
 * r0-r15 temporaries, c0-c95 float uniforms, i0-i3 int uniforms (loops),
 * b0-b15 bool uniforms, a0.x/a0.y/aL address registers, cmp.x/cmp.y.
 *
 * Arithmetic is done in float32. The hardware works with float24 (16
 * mantissa bits), so low bits can differ; its documented quirks are
 * modelled: inf * 0 = 0, no negative zero, and min/max that return the
 * second operand when one is NaN. */
#include <inttypes.h>
#include <math.h>
#include <string.h>

#include "pica.h"

#define MAX_STEPS 100000            /* instructions per run: catches a shader that never ends */

/* Stack entries for the three control-flow stacks. */
typedef struct { uint32_t end, ret; } CallEntry;               /* at `end`, go to `ret` */
typedef struct { uint32_t end, target; } IfEntry;              /* at `end`, go to `target` */
typedef struct { uint32_t end, start, remaining; int32_t inc; } LoopEntry;

static float comp(Vec4 v, int i)
{
    switch (i) {
    case 0:  return v.x;
    case 1:  return v.y;
    case 2:  return v.z;
    default: return v.w;
    }
}

static void set_comp(Vec4 *v, int i, float f)
{
    if (f == 0.0f)
        f = 0.0f;                   /* the PICA has no negative zero */
    switch (i) {
    case 0:  v->x = f; break;
    case 1:  v->y = f; break;
    case 2:  v->z = f; break;
    default: v->w = f; break;
    }
}

/* Multiplication as the hardware does it: inf * 0 = 0 (but NaN * 0 = NaN). */
static float pmul(float a, float b)
{
    if ((a == 0.0f && !isnan(b)) || (b == 0.0f && !isnan(a)))
        return 0.0f;
    return a * b;
}

static float pmax(float a, float b) { return a > b ? a : b; }
static float pmin(float a, float b) { return a < b ? a : b; }

typedef struct {
    const ShaderUnit *unit;
    ShaderRegs *s;
    uint16_t bools;
    uint8_t ints[4][4];
} Exec;

/* Read source register `reg` (raw 7-bit value), relative-addressed by
   `idx` (0 none, 1 a0.x, 2 a0.y, 3 aL; constants only), then swizzled and
   maybe negated by the descriptor's 9-bit selector field. */
static Vec4 read_src(const Exec *e, uint32_t reg, uint32_t idx, uint32_t selector_neg)
{
    Vec4 v;
    if (reg < 0x10) {
        v = e->s->v[reg];
    } else if (reg < 0x20) {
        v = e->s->r[reg - 0x10];
    } else {
        int32_t offset = idx == 1 ? e->s->a0x : idx == 2 ? e->s->a0y : idx == 3 ? e->s->aL : 0;
        if (offset < -128 || offset > 127)
            offset = 0;
        uint32_t c = (reg - 0x20 + (uint32_t)offset) & 0x7F;
        v = c > 95 ? (Vec4){ 1, 1, 1, 1 } : e->unit->uniform[c];
    }
    uint32_t sel = selector_neg >> 1 & 0xFF;
    Vec4 out = {
        comp(v, (int)(sel >> 6 & 3)), comp(v, (int)(sel >> 4 & 3)),
        comp(v, (int)(sel >> 2 & 3)), comp(v, (int)(sel & 3)),
    };
    if (selector_neg & 1) {
        out.x = -out.x;
        out.y = -out.y;
        out.z = -out.z;
        out.w = -out.w;
    }
    return out;
}

static void write_dst(Exec *e, uint32_t reg, uint32_t mask, Vec4 v)
{
    Vec4 *d = reg < 0x10 ? &e->s->o[reg] : &e->s->r[reg - 0x10];
    for (int i = 0; i < 4; i++)
        if (mask >> (3 - i) & 1)     /* mask bit 3 = x ... bit 0 = w */
            set_comp(d, i, comp(v, i));
}

static Vec4 splat(float f) { return (Vec4){ f, f, f, f }; }

static int compare(uint32_t op, float a, float b)
{
    switch (op) {
    case 0:  return a == b;
    case 1:  return a != b;
    case 2:  return a < b;
    case 3:  return a <= b;
    case 4:  return a > b;
    case 5:  return a >= b;
    default: return 1;
    }
}

static int condition(const Exec *e, uint32_t insn)
{
    int refy = insn >> 24 & 1, refx = insn >> 25 & 1;
    int x = e->s->cmpx == refx, y = e->s->cmpy == refy;
    switch (insn >> 22 & 3) {
    case 0:  return x || y;
    case 1:  return x && y;
    case 2:  return x;
    default: return y;
    }
}

void shader_exec(const ShaderUnit *unit, uint32_t entry, uint16_t bools, const uint8_t ints[4][4], ShaderRegs *s)
{
    Exec e = { unit, s, bools, { { 0 } } };
    memcpy(e.ints, ints, sizeof e.ints);
    CallEntry calls[4];
    IfEntry ifs[8];
    LoopEntry loops[4];
    int ncalls = 0, nifs = 0, nloops = 0;
    uint32_t pc = entry & 0xFFF;

    for (int steps = 0;; steps++) {
        if (steps == MAX_STEPS)
            gpu_fatal("shader still running after %d instructions (pc %03" PRIx32 ")", MAX_STEPS, pc);
        uint32_t insn = unit->code[pc], op = insn >> 26;
        uint32_t next = pc + 1, jump = UINT32_MAX;

        if (op >= 0x30) {           /* MAD / MADI: dst = src1 * src2 + src3 */
            uint32_t desc = unit->opdesc[insn & 0x1F], dst = insn >> 24 & 0x1F, idx = insn >> 22 & 3;
            uint32_t src1 = insn >> 17 & 0x1F, src2, src3;
            int inverted = (insn >> 29) == 6;
            if (inverted) {         /* MADI: the constant may be src3 */
                src2 = insn >> 12 & 0x1F;
                src3 = insn >> 5 & 0x7F;
            } else {
                src2 = insn >> 10 & 0x7F;
                src3 = insn >> 5 & 0x1F;
            }
            Vec4 a = read_src(&e, src1, 0, desc >> 4 & 0x1FF);
            Vec4 b = read_src(&e, src2, inverted ? 0 : idx, desc >> 13 & 0x1FF);
            Vec4 c = read_src(&e, src3, inverted ? idx : 0, desc >> 22 & 0x1FF);
            Vec4 r = { pmul(a.x, b.x) + c.x, pmul(a.y, b.y) + c.y, pmul(a.z, b.z) + c.z, pmul(a.w, b.w) + c.w };
            write_dst(&e, dst, desc & 0xF, r);
        } else if (op < 0x20) {     /* formats 1, 1i and 1u: arithmetic */
            uint32_t desc = unit->opdesc[insn & 0x7F], dst = insn >> 21 & 0x1F, idx = insn >> 19 & 3;
            int inverted = op >= 0x18 && op <= 0x1B;
            uint32_t src1 = inverted ? insn >> 14 & 0x1F : insn >> 12 & 0x7F;
            uint32_t src2 = inverted ? insn >> 7 & 0x7F : insn >> 7 & 0x1F;
            Vec4 a = read_src(&e, src1, inverted ? 0 : idx, desc >> 4 & 0x1FF);
            Vec4 b = read_src(&e, src2, inverted ? idx : 0, desc >> 13 & 0x1FF);
            uint32_t mask = desc & 0xF;
            Vec4 r;
            switch (op) {
            case 0x00: r = (Vec4){ a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w }; break;           /* ADD */
            case 0x01: r = splat(pmul(a.x, b.x) + pmul(a.y, b.y) + pmul(a.z, b.z)); break;      /* DP3 */
            case 0x02: r = splat(pmul(a.x, b.x) + pmul(a.y, b.y) + pmul(a.z, b.z) + pmul(a.w, b.w)); break;
            case 0x03:                                                                          /* DPH */
            case 0x18: r = splat(pmul(a.x, b.x) + pmul(a.y, b.y) + pmul(a.z, b.z) + b.w); break;  /* DPHI */
            case 0x04:                                                                          /* DST */
            case 0x19: r = (Vec4){ 1.0f, pmul(a.y, b.y), a.z, b.w }; break;                     /* DSTI */
            case 0x05: r = splat(exp2f(a.x)); break;                                            /* EX2 */
            case 0x06: r = splat(log2f(a.x)); break;                                            /* LG2 */
            case 0x07:                                                                          /* LITP */
                r = (Vec4){ pmax(a.x, 0), pmax(pmin(a.y, 127.9961f), -127.9961f), 0, pmax(a.w, 0) };
                s->cmpx = a.x >= 0;
                s->cmpy = a.w >= 0;
                break;
            case 0x08: r = (Vec4){ pmul(a.x, b.x), pmul(a.y, b.y), pmul(a.z, b.z), pmul(a.w, b.w) }; break;
            case 0x09:                                                                          /* SGE */
            case 0x1A: r = (Vec4){ a.x >= b.x, a.y >= b.y, a.z >= b.z, a.w >= b.w }; break;     /* SGEI */
            case 0x0A:                                                                          /* SLT */
            case 0x1B: r = (Vec4){ a.x < b.x, a.y < b.y, a.z < b.z, a.w < b.w }; break;         /* SLTI */
            case 0x0B: r = (Vec4){ floorf(a.x), floorf(a.y), floorf(a.z), floorf(a.w) }; break; /* FLR */
            case 0x0C: r = (Vec4){ pmax(a.x, b.x), pmax(a.y, b.y), pmax(a.z, b.z), pmax(a.w, b.w) }; break;
            case 0x0D: r = (Vec4){ pmin(a.x, b.x), pmin(a.y, b.y), pmin(a.z, b.z), pmin(a.w, b.w) }; break;
            case 0x0E: r = splat(a.x == 0 ? INFINITY : 1.0f / a.x); break;                      /* RCP */
            case 0x0F: r = splat(a.x == 0 ? INFINITY : 1.0f / sqrtf(a.x)); break;               /* RSQ */
            case 0x12:                                                                          /* MOVA */
                if (mask & 8)
                    s->a0x = (int32_t)a.x;
                if (mask & 4)
                    s->a0y = (int32_t)a.y;
                pc = next;
                goto stacks;
            case 0x13: r = a; break;                                                            /* MOV */
            default:
                gpu_fatal("shader instruction %08" PRIx32 " (opcode 0x%02" PRIx32 ") at %03" PRIx32 " is unknown",
                          insn, op, pc);
            }
            write_dst(&e, dst, mask, r);
        } else if ((op >> 1) == 0x17) {     /* CMP: format 1c */
            uint32_t desc = unit->opdesc[insn & 0x7F];
            Vec4 a = read_src(&e, insn >> 12 & 0x7F, insn >> 19 & 3, desc >> 4 & 0x1FF);
            Vec4 b = read_src(&e, insn >> 7 & 0x1F, 0, desc >> 13 & 0x1FF);
            s->cmpx = compare(insn >> 24 & 7, a.x, b.x);
            s->cmpy = compare(insn >> 21 & 7, a.y, b.y);
        } else {                            /* flow control: formats 0, 2, 3 */
            uint32_t num = insn & 0xFF, dst = insn >> 10 & 0xFFF, id = insn >> 22 & 0xF;
            switch (op) {
            case 0x20:                      /* BREAK */
            case 0x23:                      /* BREAKC */
                if (op == 0x20 || condition(&e, insn)) {
                    if (!nloops)
                        gpu_fatal("shader BREAK outside a loop at %03" PRIx32, pc);
                    jump = loops[--nloops].end;
                }
                break;
            case 0x21:                      /* NOP */
                break;
            case 0x22:                      /* END */
                return;
            case 0x24:                      /* CALL */
            case 0x25:                      /* CALLC */
            case 0x26:                      /* CALLU */
                if (op == 0x24 || (op == 0x25 && condition(&e, insn)) || (op == 0x26 && (e.bools >> id & 1))) {
                    if (ncalls == 4)
                        gpu_fatal("shader call stack overflow at %03" PRIx32, pc);
                    calls[ncalls++] = (CallEntry){ dst + num, next };
                    next = dst;
                }
                break;
            case 0x27:                      /* IFU */
            case 0x28:                      /* IFC */
                if (op == 0x27 ? (e.bools >> id & 1) : condition(&e, insn)) {
                    if (nifs == 8)
                        gpu_fatal("shader if stack overflow at %03" PRIx32, pc);
                    ifs[nifs++] = (IfEntry){ dst, dst + num };
                } else {
                    next = dst;
                }
                break;
            case 0x29: {                    /* LOOP: i.x + 1 times, aL = i.y, then += i.z */
                const uint8_t *i = e.ints[id & 3];
                if (nloops == 4)
                    gpu_fatal("shader loop stack overflow at %03" PRIx32, pc);
                loops[nloops++] = (LoopEntry){ dst + 1, next, (uint32_t)i[0] + 1, (int8_t)i[2] };
                s->aL = i[1];
                break;
            }
            case 0x2C:                      /* JMPC */
                if (condition(&e, insn))
                    jump = dst;
                break;
            case 0x2D:                      /* JMPU: bit 0 of NUM inverts the test */
                if ((e.bools >> id & 1) != (num & 1))
                    jump = dst;
                break;
            case 0x2A:                      /* EMIT */
            case 0x2B:                      /* SETEMIT */
                gpu_fatal("geometry shader instruction %08" PRIx32 " at %03" PRIx32 " (geometry shaders come later)",
                          insn, pc);
            default:
                gpu_fatal("shader instruction %08" PRIx32 " (opcode 0x%02" PRIx32 ") at %03" PRIx32 " is unknown",
                          insn, op, pc);
            }
        }
        pc = next;

    stacks:
        /* After each instruction the stacks check the new pc: a loop body,
           an if block or a call can end here. Loop wins over if over call
           over a jump (3dbrew's Control Flow). */
        {
            uint32_t call_pc = pc, if_pc = pc, loop_pc = pc;
            int call_hit = 0, if_hit = 0, loop_hit = 0;
            while (ncalls && calls[ncalls - 1].end == call_pc) {
                call_pc = calls[--ncalls].ret;
                call_hit = 1;
            }
            if (nifs && ifs[nifs - 1].end == if_pc) {
                if_pc = ifs[--nifs].target;
                if_hit = 1;
            }
            if (nloops && loops[nloops - 1].end == loop_pc) {
                LoopEntry *l = &loops[nloops - 1];
                s->aL += l->inc;
                if (--l->remaining)
                    loop_pc = l->start;
                else
                    nloops--;
                loop_hit = 1;
            }
            if (loop_hit)
                pc = loop_pc;
            else if (if_hit)
                pc = if_pc;
            else if (call_hit)
                pc = call_pc;
            else if (jump != UINT32_MAX)
                pc = jump;
            if (pc >= 4096)
                gpu_fatal("shader pc ran off the program (%03" PRIx32 ")", pc);
        }
    }
}
