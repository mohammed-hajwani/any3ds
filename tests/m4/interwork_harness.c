/* M4 interworking harness: recompiled mixed ARM/Thumb program vs the same
 * two C files compiled natively. */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "context.h"
#include "interwork_arm.c"
#include "interwork_thumb.c"

typedef uint32_t (*RefFn)(uint32_t a, uint32_t b);

static uint32_t r_a_double(uint32_t a, uint32_t b)   { (void)b; return a_double(a); }
static uint32_t r_a_calls_t(uint32_t a, uint32_t b)  { (void)b; return a_calls_t(a); }
static uint32_t r_a_nested(uint32_t a, uint32_t b)   { (void)b; return a_nested(a); }
static uint32_t r_a_deep(uint32_t a, uint32_t b)     { return a_deep(a, b % 40); }
static uint32_t r_t_square(uint32_t a, uint32_t b)   { (void)b; return t_square(a); }
static uint32_t r_t_calls_a(uint32_t a, uint32_t b)  { (void)b; return t_calls_a(a); }
static uint32_t r_t_deep(uint32_t a, uint32_t b)     { return t_deep(a, b % 40); }
static uint32_t r_t_sum_both(uint32_t a, uint32_t b) { return t_sum_both(a, b); }

static const struct { const char *name; RefFn ref; int bounded; } tests[] = {
    { "a_double", r_a_double, 0 },  { "a_calls_t", r_a_calls_t, 0 }, { "a_nested", r_a_nested, 0 },
    { "a_deep", r_a_deep, 1 },      { "t_square", r_t_square, 0 },   { "t_calls_a", r_t_calls_a, 0 },
    { "t_deep", r_t_deep, 1 },      { "t_sum_both", r_t_sum_both, 0 },
};

static uint32_t rng_state = 0xBADC0DEu;
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
        int fails = 0, runs = 2000;
        for (int k = 0; k < runs; k++) {
            uint32_t a = rnd(), b = rnd();
            if (tests[ti].bounded)
                b %= 40;                        /* recursion depth */
            Ctx ctx;
            memset(&ctx, 0, sizeof ctx);
            for (int r = 0; r < 16; r++)
                ctx.r[r] = 0xDEAD0000u | (uint32_t)r;
            ctx.r[0] = a;
            ctx.r[1] = b;
            ctx.r[13] = 0x10000000u;
            ctx.r[14] = 0x00ABC000u;            /* an ARM return address (bit 0 clear) */
            ctx.mem = mem;
            Ctx before = ctx;
            fn(&ctx);
            uint32_t want = tests[ti].ref(a, b);
            int ok = ctx.r[0] == want;
            for (int r = 4; r <= 13; r++)
                ok &= r == 12 || ctx.r[r] == before.r[r];
            if (!ok && fails++ < 3)
                printf("FAIL %s(%08" PRIx32 ", %08" PRIx32 ") = %08" PRIx32 ", want %08" PRIx32 "\n",
                       tests[ti].name, a, b, ctx.r[0], want);
        }
        printf("%s %-10s %d/%d\n", fails ? "FAIL" : "PASS", tests[ti].name, runs - fails, runs);
        failed += fails != 0;
    }
    guest_mem_free(mem);
    if (failed) {
        printf("%d function(s) failed\n", failed);
        return 1;
    }
    printf("all interworking calls match the native reference\n");
    return 0;
}
