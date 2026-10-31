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
 * second operand when one is NaN.
 *
 * Speed: every vertex runs the program, so each instruction is decoded once
 * (its sources, swizzles, negation and write mask) into a table per unit,
 * which shader_prepare fills when the program or its descriptors change.
 * An entry is only used while its instruction and descriptor words still
 * match, so a stale table never changes results. */
#include <inttypes.h>
#include <math.h>
#include <string.h>

#include "pica.h"

#define MAX_STEPS 100000            /* instructions per run: catches a shader that never ends */

/* Stack entries for the three control-flow stacks. */
typedef struct { uint32_t end, ret; } CallEntry;               /* at `end`, go to `ret` */
typedef struct { uint32_t end, target; } IfEntry;              /* at `end`, go to `target` */
typedef struct { uint32_t end, start, remaining; int32_t inc; } LoopEntry;

/* A decoded source operand: register file (0 v, 1 r, 2 c), register (for
   c: before relative addressing), the relative index (0 none, 1 a0.x,
   2 a0.y, 3 aL; constants only), the swizzle and the negate bit. */
typedef struct { uint8_t file, reg, rel, neg, swz[4]; } Src;

enum { K_MAD, K_ARITH, K_CMP, K_FLOW };

typedef struct {
    uint32_t insn, desc;            /* the words it was decoded from */
    uint8_t kind, op, dst, mask;
    uint8_t desc_index;             /* 0xFF: no descriptor (flow control) */
    Src src[3];
} Decoded;

/* Multiplication as the hardware does it: inf * 0 = 0 (but NaN * 0 = NaN). */
static float pmul(float a, float b)
{
    if ((a == 0.0f && !isnan(b)) || (b == 0.0f && !isnan(a)))
        return 0.0f;
    return a * b;
}

static float pmax(float a, float b) { return a > b ? a : b; }
static float pmin(float a, float b) { return a < b ? a : b; }

/* Source `reg` (raw 7-bit value), relative-addressed by `idx`, swizzled and
   maybe negated by the descriptor's 9-bit selector field. */
static Src decode_src(uint32_t reg, uint32_t idx, uint32_t selector_neg)
{
    Src s;
    if (reg < 0x10) {
        s.file = 0;
        s.reg = (uint8_t)reg;
        s.rel = 0;
    } else if (reg < 0x20) {
        s.file = 1;
        s.reg = (uint8_t)(reg - 0x10);
        s.rel = 0;
    } else {
        s.file = 2;
        s.reg = (uint8_t)(reg - 0x20);
        s.rel = (uint8_t)idx;
    }
    uint32_t sel = selector_neg >> 1 & 0xFF;
    s.swz[0] = (uint8_t)(sel >> 6 & 3);
    s.swz[1] = (uint8_t)(sel >> 4 & 3);
    s.swz[2] = (uint8_t)(sel >> 2 & 3);
    s.swz[3] = (uint8_t)(sel & 3);
    s.neg = (uint8_t)(selector_neg & 1);
    return s;
}

static Decoded decode(const ShaderUnit *unit, uint32_t pc)
{
    Decoded d;
    memset(&d, 0, sizeof d);
    uint32_t insn = unit->code[pc], op = insn >> 26;
    d.insn = insn;
    d.op = (uint8_t)op;
    d.desc_index = 0xFF;
    if (op >= 0x30) {               /* MAD / MADI: dst = src1 * src2 + src3 */
        d.kind = K_MAD;
        d.desc_index = (uint8_t)(insn & 0x1F);
        uint32_t desc = unit->opdesc[d.desc_index], idx = insn >> 22 & 3;
        d.desc = desc;
        d.dst = (uint8_t)(insn >> 24 & 0x1F);
        d.mask = (uint8_t)(desc & 0xF);
        uint32_t src1 = insn >> 17 & 0x1F, src2, src3;
        int inverted = (insn >> 29) == 6;
        if (inverted) {             /* MADI: the constant may be src3 */
            src2 = insn >> 12 & 0x1F;
            src3 = insn >> 5 & 0x7F;
        } else {
            src2 = insn >> 10 & 0x7F;
            src3 = insn >> 5 & 0x1F;
        }
        d.src[0] = decode_src(src1, 0, desc >> 4 & 0x1FF);
        d.src[1] = decode_src(src2, inverted ? 0 : idx, desc >> 13 & 0x1FF);
        d.src[2] = decode_src(src3, inverted ? idx : 0, desc >> 22 & 0x1FF);
    } else if (op < 0x20) {         /* formats 1, 1i and 1u: arithmetic */
        d.kind = K_ARITH;
        d.desc_index = (uint8_t)(insn & 0x7F);
        uint32_t desc = unit->opdesc[d.desc_index], idx = insn >> 19 & 3;
        d.desc = desc;
        d.dst = (uint8_t)(insn >> 21 & 0x1F);
        d.mask = (uint8_t)(desc & 0xF);
        int inverted = op >= 0x18 && op <= 0x1B;
        uint32_t src1 = inverted ? insn >> 14 & 0x1F : insn >> 12 & 0x7F;
        uint32_t src2 = inverted ? insn >> 7 & 0x7F : insn >> 7 & 0x1F;
        d.src[0] = decode_src(src1, inverted ? 0 : idx, desc >> 4 & 0x1FF);
        d.src[1] = decode_src(src2, inverted ? idx : 0, desc >> 13 & 0x1FF);
    } else if ((op >> 1) == 0x17) { /* CMP: format 1c */
        d.kind = K_CMP;
        d.desc_index = (uint8_t)(insn & 0x7F);
        uint32_t desc = unit->opdesc[d.desc_index];
        d.desc = desc;
        d.src[0] = decode_src(insn >> 12 & 0x7F, insn >> 19 & 3, desc >> 4 & 0x1FF);
        d.src[1] = decode_src(insn >> 7 & 0x1F, 0, desc >> 13 & 0x1FF);
    } else {                        /* flow control: formats 0, 2, 3 */
        d.kind = K_FLOW;
    }
    return d;
}

static int still_valid(const ShaderUnit *unit, const Decoded *d, uint32_t pc)
{
    return d->insn == unit->code[pc] && (d->desc_index == 0xFF || d->desc == unit->opdesc[d->desc_index]);
}

/* The decoded tables of the two units in `pica`. Other units (tests) are
   decoded instruction by instruction. */
static struct { const ShaderUnit *unit; uint32_t version, filled; Decoded d[4096]; } tables[2];

static Decoded *table_of(const ShaderUnit *unit)
{
    if (unit == &pica.vsh)
        return tables[0].d;
    if (unit == &pica.gsh)
        return tables[1].d;
    return NULL;
}

void shader_prepare(const ShaderUnit *unit)
{
    int t = unit == &pica.vsh ? 0 : unit == &pica.gsh ? 1 : -1;
    if (t < 0 || (tables[t].filled && tables[t].version == unit->version))
        return;
    for (uint32_t pc = 0; pc < 4096; pc++)
        if (!tables[t].filled || !still_valid(unit, &tables[t].d[pc], pc))
            tables[t].d[pc] = decode(unit, pc);
    tables[t].version = unit->version;
    tables[t].filled = 1;
}

typedef struct {
    const ShaderUnit *unit;
    ShaderRegs *s;
    uint16_t bools;
    uint8_t ints[4][4];
} Exec;

static const float ones[4] = { 1, 1, 1, 1 };

static void read_src(const Exec *e, const Src *src, float out[4])
{
    const float *v;
    if (src->file == 0) {
        v = &e->s->v[src->reg].x;
    } else if (src->file == 1) {
        v = &e->s->r[src->reg].x;
    } else {
        int32_t offset = src->rel == 1 ? e->s->a0x : src->rel == 2 ? e->s->a0y : src->rel == 3 ? e->s->aL : 0;
        if (offset < -128 || offset > 127)
            offset = 0;
        uint32_t c = (src->reg + (uint32_t)offset) & 0x7F;
        v = c > 95 ? ones : &e->unit->uniform[c].x;
    }
    out[0] = v[src->swz[0]];
    out[1] = v[src->swz[1]];
    out[2] = v[src->swz[2]];
    out[3] = v[src->swz[3]];
    if (src->neg) {
        out[0] = -out[0];
        out[1] = -out[1];
        out[2] = -out[2];
        out[3] = -out[3];
    }
}

static void write_dst(Exec *e, uint32_t reg, uint32_t mask, const float v[4])
{
    float *d = reg < 0x10 ? &e->s->o[reg].x : &e->s->r[reg - 0x10].x;
    for (int i = 0; i < 4; i++)
        if (mask >> (3 - i) & 1) {   /* mask bit 3 = x ... bit 0 = w */
            float f = v[i];
            if (f == 0.0f)
                f = 0.0f;           /* the PICA has no negative zero */
            d[i] = f;
        }
}

static void splat(float r[4], float f) { r[0] = r[1] = r[2] = r[3] = f; }

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
    const Decoded *table = table_of(unit);
    CallEntry calls[4];
    IfEntry ifs[8];
    LoopEntry loops[4];
    int ncalls = 0, nifs = 0, nloops = 0;
    uint32_t pc = entry & 0xFFF;

    for (int steps = 0;; steps++) {
        if (steps == MAX_STEPS)
            gpu_fatal("shader still running after %d instructions (pc %03" PRIx32 ")", MAX_STEPS, pc);
        Decoded fresh;
        const Decoded *d = table ? &table[pc] : NULL;
        if (!d || !still_valid(unit, d, pc)) {
            fresh = decode(unit, pc);
            d = &fresh;
        }
        uint32_t insn = d->insn, op = d->op;
        uint32_t next = pc + 1, jump = UINT32_MAX;

        if (d->kind == K_MAD) {
            float a[4], b[4], c[4], r[4];
            read_src(&e, &d->src[0], a);
            read_src(&e, &d->src[1], b);
            read_src(&e, &d->src[2], c);
            for (int i = 0; i < 4; i++)
                r[i] = pmul(a[i], b[i]) + c[i];
            write_dst(&e, d->dst, d->mask, r);
        } else if (d->kind == K_ARITH) {
            float a[4], b[4], r[4];
            read_src(&e, &d->src[0], a);
            read_src(&e, &d->src[1], b);
            uint32_t mask = d->mask;
            switch (op) {
            case 0x00: for (int i = 0; i < 4; i++) r[i] = a[i] + b[i]; break;                   /* ADD */
            case 0x01: splat(r, pmul(a[0], b[0]) + pmul(a[1], b[1]) + pmul(a[2], b[2])); break; /* DP3 */
            case 0x02: splat(r, pmul(a[0], b[0]) + pmul(a[1], b[1]) + pmul(a[2], b[2]) + pmul(a[3], b[3])); break;
            case 0x03:                                                                          /* DPH */
            case 0x18: splat(r, pmul(a[0], b[0]) + pmul(a[1], b[1]) + pmul(a[2], b[2]) + b[3]); break;  /* DPHI */
            case 0x04:                                                                          /* DST */
            case 0x19: r[0] = 1.0f; r[1] = pmul(a[1], b[1]); r[2] = a[2]; r[3] = b[3]; break;  /* DSTI */
            case 0x05: splat(r, exp2f(a[0])); break;                                           /* EX2 */
            case 0x06: splat(r, log2f(a[0])); break;                                           /* LG2 */
            case 0x07:                                                                          /* LITP */
                r[0] = pmax(a[0], 0);
                r[1] = pmax(pmin(a[1], 127.9961f), -127.9961f);
                r[2] = 0;
                r[3] = pmax(a[3], 0);
                s->cmpx = a[0] >= 0;
                s->cmpy = a[3] >= 0;
                break;
            case 0x08: for (int i = 0; i < 4; i++) r[i] = pmul(a[i], b[i]); break;              /* MUL */
            case 0x09:                                                                          /* SGE */
            case 0x1A: for (int i = 0; i < 4; i++) r[i] = a[i] >= b[i]; break;                 /* SGEI */
            case 0x0A:                                                                          /* SLT */
            case 0x1B: for (int i = 0; i < 4; i++) r[i] = a[i] < b[i]; break;                  /* SLTI */
            case 0x0B: for (int i = 0; i < 4; i++) r[i] = floorf(a[i]); break;                 /* FLR */
            case 0x0C: for (int i = 0; i < 4; i++) r[i] = pmax(a[i], b[i]); break;             /* MAX */
            case 0x0D: for (int i = 0; i < 4; i++) r[i] = pmin(a[i], b[i]); break;             /* MIN */
            case 0x0E: splat(r, a[0] == 0 ? INFINITY : 1.0f / a[0]); break;                    /* RCP */
            case 0x0F: splat(r, a[0] == 0 ? INFINITY : 1.0f / sqrtf(a[0])); break;             /* RSQ */
            case 0x12:                                                                          /* MOVA */
                if (mask & 8)
                    s->a0x = (int32_t)a[0];
                if (mask & 4)
                    s->a0y = (int32_t)a[1];
                pc = next;
                goto stacks;
            case 0x13: memcpy(r, a, sizeof r); break;                                          /* MOV */
            default:
                gpu_fatal("shader instruction %08" PRIx32 " (opcode 0x%02" PRIx32 ") at %03" PRIx32 " is unknown",
                          insn, op, pc);
            }
            write_dst(&e, d->dst, mask, r);
        } else if (d->kind == K_CMP) {
            float a[4], b[4];
            read_src(&e, &d->src[0], a);
            read_src(&e, &d->src[1], b);
            s->cmpx = compare(insn >> 24 & 7, a[0], b[0]);
            s->cmpy = compare(insn >> 21 & 7, a[1], b[1]);
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
            case 0x2A:                      /* EMIT (geometry shaders) */
                if (!s->on_emit)
                    gpu_fatal("EMIT at %03" PRIx32 " outside a geometry shader", pc);
                s->on_emit(s);
                break;
            case 0x2B:                      /* SETEMIT: vertex slot, primitive and winding flags */
                s->emit_vertex = (int)(insn >> 24 & 3);
                s->emit_prim = (int)(insn >> 23 & 1);
                s->emit_winding = (int)(insn >> 22 & 1);
                break;
            default:
                gpu_fatal("shader instruction %08" PRIx32 " (opcode 0x%02" PRIx32 ") at %03" PRIx32 " is unknown",
                          insn, op, pc);
            }
        }
        pc = next;

    stacks:
        if (!ncalls && !nifs && !nloops) {  /* nothing can end here: only a jump */
            if (jump != UINT32_MAX)
                pc = jump;
            if (pc >= 4096)
                gpu_fatal("shader pc ran off the program (%03" PRIx32 ")", pc);
            continue;
        }
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
