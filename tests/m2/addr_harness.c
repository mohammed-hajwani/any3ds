/* Harness for addr.S: run each addressing-mode function on random memory
 * and compare r0-r3 and the scratch buffer with a C model of the
 * instruction (written from the ARM ARM, not from the translator). */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "context.h"

#define STACK_TOP 0x10000000u
#define SCRATCH   0x08000000u
#define SIZE      0x200u
#define BASE      0x100u        /* r0 points here, so negative offsets stay inside */

typedef struct { const char *name; uint32_t r2; } Case;   /* r2: index register input */

static const Case cases[] = {
    { "ldr_imm", 0 },   { "ldr_neg", 0 },    { "ldr_reg", 12 },  { "ldr_regneg", 8 },
    { "ldr_lsl", 3 },   { "ldr_pre", 0 },    { "ldr_post", 0 },  { "ldr_postreg", 8 },
    { "ldrb_", 0 },     { "ldrb_preneg", 0 }, { "ldrh_", 0 },    { "ldrsb_", 0 },
    { "ldrsh_", 0 },    { "ldrsh_reg", 6 },
    { "str_imm", 0 },   { "str_regpre", 16 }, { "strb_", 0 },    { "strb_post", 0 },
    { "strh_", 0 },     { "strh_post", 0 },
    { "ldm_ia", 0 },    { "ldm_ia_wb", 0 },  { "ldmib_", 0 },    { "ldmda_", 0 },
    { "ldmdb_wb", 0 },  { "ldm_base", 0 },
    { "stm_ia", 0 },    { "stmib_wb", 0 },   { "stmda_", 0 },    { "stmdb_wb", 0 },
    { "push_pop", 0 },  { "push1_pop1", 0 }, { "pool", 0 },      { "pool_data", 0 },
};

/* ---- model: memory is m[] (offsets relative to SCRATCH) ---- */

static uint8_t m[SIZE];
static uint32_t rd32(uint32_t o) { uint32_t v; memcpy(&v, m + o, 4); return v; }
static uint32_t rd16(uint32_t o) { uint16_t v; memcpy(&v, m + o, 2); return v; }
static void wr32(uint32_t o, uint32_t v) { memcpy(m + o, &v, 4); }
static void wr16(uint32_t o, uint32_t v) { uint16_t h = (uint16_t)v; memcpy(m + o, &h, 2); }
static uint32_t sext8(uint32_t v)  { return (v & 0x80) ? v | 0xFFFFFF00u : v; }
static uint32_t sext16(uint32_t v) { return (v & 0x8000) ? v | 0xFFFF0000u : v; }

static void model(const char *n, uint32_t *R)
{
    uint32_t o = R[0] - SCRATCH;     /* offset of the base pointer */
#define IS(x) (strcmp(n, x) == 0)
    if      (IS("ldr_imm"))     R[1] = rd32(o + 8);
    else if (IS("ldr_neg"))     R[1] = rd32(o - 4);
    else if (IS("ldr_reg"))     R[1] = rd32(o + R[2]);
    else if (IS("ldr_regneg"))  R[1] = rd32(o - R[2]);
    else if (IS("ldr_lsl"))     R[1] = rd32(o + 4 * R[2]);
    else if (IS("ldr_pre"))   { R[1] = rd32(o + 4); R[0] += 4; }
    else if (IS("ldr_post"))  { R[1] = rd32(o);     R[0] += 4; }
    else if (IS("ldr_postreg")) { R[1] = rd32(o);   R[0] -= R[2]; }
    else if (IS("ldrb_"))       R[1] = m[o + 3];
    else if (IS("ldrb_preneg")) { R[1] = m[o - 1];  R[0] -= 1; }
    else if (IS("ldrh_"))       R[1] = rd16(o + 2);
    else if (IS("ldrsb_"))      R[1] = sext8(m[o + 5]);
    else if (IS("ldrsh_"))      R[1] = sext16(rd16(o + 6));
    else if (IS("ldrsh_reg"))   R[1] = sext16(rd16(o + R[2]));
    else if (IS("str_imm"))     wr32(o + 8, R[1]);
    else if (IS("str_regpre"))  { wr32(o + R[2], R[1]); R[0] += R[2]; }
    else if (IS("strb_"))       m[o + 3] = (uint8_t)R[1];
    else if (IS("strb_post"))   { m[o] = (uint8_t)R[1]; R[0] += 1; }
    else if (IS("strh_"))       wr16(o + 2, R[1]);
    else if (IS("strh_post"))   { wr16(o, R[1]); R[0] += 2; }
    else if (IS("ldm_ia"))      { R[1] = rd32(o); R[2] = rd32(o + 4); R[3] = rd32(o + 8); }
    else if (IS("ldm_ia_wb"))   { R[1] = rd32(o); R[2] = rd32(o + 4); R[0] += 8; }
    else if (IS("ldmib_"))      { R[1] = rd32(o + 4); R[2] = rd32(o + 8); }
    else if (IS("ldmda_"))      { R[1] = rd32(o - 4); R[2] = rd32(o); }
    else if (IS("ldmdb_wb"))    { R[1] = rd32(o - 8); R[2] = rd32(o - 4); R[0] -= 8; }
    else if (IS("ldm_base"))    { R[1] = rd32(o + 4); R[0] = rd32(o); }
    else if (IS("stm_ia"))      { wr32(o, R[1]); wr32(o + 4, R[2]); }
    else if (IS("stmib_wb"))    { wr32(o + 4, R[1]); wr32(o + 8, R[2]); R[0] += 8; }
    else if (IS("stmda_"))      { wr32(o - 4, R[1]); wr32(o, R[2]); }
    else if (IS("stmdb_wb"))    { wr32(o - 12, R[1]); wr32(o - 8, R[2]); wr32(o - 4, R[3]); R[0] -= 12; }
    else if (IS("push_pop"))    R[1] = R[1] + R[2];
    else if (IS("push1_pop1"))  R[2] = R[1];
    else if (IS("pool"))        R[1] = 0xCAFEF00Du;
    else if (IS("pool_data"))   R[1] = 0x600DF00Du;
#undef IS
}

static uint32_t rng_state = 0x9E3779B9u;
static uint32_t rnd(void)
{
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rng_state = x;
}

int main(void)
{
    uint8_t *mem = guest_mem_alloc();
    recomp_load_image(mem);
    int failed = 0, checks = 0;

    for (size_t ci = 0; ci < sizeof cases / sizeof cases[0]; ci++) {
        const Case *tc = &cases[ci];
        GuestFn fn = recomp_lookup(tc->name);
        if (!fn) { printf("FAIL %s: not found\n", tc->name); failed++; continue; }

        int fails = 0;
        for (int iter = 0; iter < 200; iter++) {
            for (uint32_t i = 0; i < SIZE; i++)
                m[i] = (uint8_t)rnd();
            memcpy(mem + SCRATCH, m, SIZE);

            Ctx ctx;
            memset(&ctx, 0, sizeof ctx);
            for (int r = 0; r < 16; r++)
                ctx.r[r] = 0xDEAD0000u | (uint32_t)r;
            ctx.r[0] = SCRATCH + BASE;
            ctx.r[1] = rnd();
            ctx.r[2] = tc->r2 ? tc->r2 : rnd();
            ctx.r[3] = rnd();
            ctx.r[13] = STACK_TOP - 32;
            ctx.r[14] = 0x00ABC000u;
            ctx.mem = mem;
            Ctx before = ctx;

            uint32_t want[4] = { ctx.r[0], ctx.r[1], ctx.r[2], ctx.r[3] };
            model(tc->name, want);
            fn(&ctx);
            checks++;

            int ok = memcmp(mem + SCRATCH, m, SIZE) == 0 && ctx.r[13] == before.r[13];
            for (int r = 0; r < 4; r++)
                ok &= ctx.r[r] == want[r];
            for (int r = 4; r <= 11; r++)
                ok &= ctx.r[r] == before.r[r];
            if (!ok && fails++ < 3)
                printf("FAIL %s: r0-r3 = %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
                       ", want %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 "%s\n",
                       tc->name, ctx.r[0], ctx.r[1], ctx.r[2], ctx.r[3],
                       want[0], want[1], want[2], want[3],
                       memcmp(mem + SCRATCH, m, SIZE) ? " (memory differs)" : "");
        }
        printf("%s %s\n", fails ? "FAIL" : "PASS", tc->name);
        failed += fails != 0;
    }

    guest_mem_free(mem);
    if (failed) {
        printf("%d addressing test(s) failed\n", failed);
        return 1;
    }
    printf("all %d addressing-mode checks passed\n", checks);
    return 0;
}
