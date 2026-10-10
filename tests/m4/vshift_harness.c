/* M4 harness: recompiled vshift.c (ARM or Thumb build) vs the same C
 * compiled natively, over edge and random inputs. */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "context.h"
#include "vshift.c"

typedef uint32_t (*RefFn)(const uint32_t *a);

static uint32_t ref_shl(const uint32_t *a)     { return shl(a[0], a[1]); }
static uint32_t ref_shr(const uint32_t *a)     { return shr(a[0], a[1]); }
static uint32_t ref_sar(const uint32_t *a)     { return (uint32_t)sar((int32_t)a[0], a[1]); }
static uint32_t ref_rotl(const uint32_t *a)    { return rotl(a[0], a[1]); }
static uint32_t ref_extract(const uint32_t *a) { return extract(a[0], a[1], a[2]); }
static uint32_t ref_mulhash(const uint32_t *a) { return mulhash(a[0]); }
static uint32_t ref_poly(const uint32_t *a)    { return poly(a[0], a[1], a[2]); }
static uint32_t ref_fact(const uint32_t *a)    { return fact(a[0]); }
static uint32_t ref_spread(const uint32_t *a)  { return spread(a[0]); }

static const struct { const char *name; int nargs; RefFn ref; } tests[] = {
    { "shl", 2, ref_shl },         { "shr", 2, ref_shr },     { "sar", 2, ref_sar },
    { "rotl", 2, ref_rotl },       { "extract", 3, ref_extract },
    { "mulhash", 1, ref_mulhash }, { "poly", 3, ref_poly },   { "fact", 1, ref_fact },
    { "spread", 1, ref_spread },
};

static const uint32_t edges[] = { 0, 1, 2, 7, 31, 32, 33, 63, 255, 256, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF };
#define NEDGES (sizeof edges / sizeof edges[0])

static uint32_t rng_state = 0x1234ABCDu;
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
    int failed = 0;

    for (size_t ti = 0; ti < sizeof tests / sizeof tests[0]; ti++) {
        GuestFn fn = recomp_lookup(tests[ti].name);
        if (!fn) { printf("FAIL %s: not found\n", tests[ti].name); failed++; continue; }

        int runs = 0, fails = 0;
        size_t combos = 1;
        for (int i = 0; i < tests[ti].nargs; i++)
            combos *= NEDGES;
        for (size_t k = 0; k < combos + 2000; k++) {
            uint32_t args[3] = { 0, 0, 0 };
            size_t rest = k;
            for (int i = 0; i < tests[ti].nargs; i++) {
                args[i] = k < combos ? edges[rest % NEDGES] : rnd();
                rest /= NEDGES;
            }
            Ctx ctx;
            memset(&ctx, 0, sizeof ctx);
            for (int r = 0; r < 16; r++)
                ctx.r[r] = 0xDEAD0000u | (uint32_t)r;
            memcpy(ctx.r, args, sizeof args);
            ctx.r[13] = 0x10000000u;
            ctx.r[14] = 0x00ABC000u;
            ctx.mem = mem;
            Ctx before = ctx;
            fn(&ctx);
            runs++;

            uint32_t want = tests[ti].ref(args);
            int ok = ctx.r[0] == want;
            for (int r = 4; r <= 13; r++)
                ok &= r == 12 || ctx.r[r] == before.r[r];
            if (!ok && fails++ < 3)
                printf("FAIL %s(%08" PRIx32 ", %08" PRIx32 ", %08" PRIx32 ") = %08" PRIx32 ", want %08" PRIx32 "\n",
                       tests[ti].name, args[0], args[1], args[2], ctx.r[0], want);
        }
        printf("%s %-8s %d/%d\n", fails ? "FAIL" : "PASS", tests[ti].name, runs - fails, runs);
        failed += fails != 0;
    }
    guest_mem_free(mem);
    if (failed) {
        printf("%d function(s) failed\n", failed);
        return 1;
    }
    printf("all functions match the native reference\n");
    return 0;
}
