/* M1 harness: differential test of recompiled funcs.c against the same
 * source compiled natively for the host.
 *
 * funcs.c is #included, so e.g. gcd() below is the host-native reference,
 * while recomp_lookup("gcd") returns the recompiled ARM version (named
 * f_XXXXXXXX, so the two never clash).
 */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "context.h"
#include "funcs.c"

#define STACK_TOP    0x10000000u    /* top of the 3DS homebrew stack */
#define RETURN_ADDR  0x00ABC000u    /* arbitrary: never executed, only compared */

/* Reference wrappers: unpack raw register values into each signature. */
typedef uint64_t (*RefFn)(const uint32_t *a);

static uint64_t lo_hi(uint32_t lo, uint32_t hi) { return lo | (uint64_t)hi << 32; }

static uint64_t ref_smax(const uint32_t *a)     { return (uint32_t)smax((int32_t)a[0], (int32_t)a[1]); }
static uint64_t ref_umin(const uint32_t *a)     { return umin(a[0], a[1]); }
static uint64_t ref_iabs(const uint32_t *a)     { return (uint32_t)iabs((int32_t)a[0]); }
static uint64_t ref_clamp(const uint32_t *a)    { return (uint32_t)clamp((int32_t)a[0], (int32_t)a[1], (int32_t)a[2]); }
static uint64_t ref_gcd(const uint32_t *a)      { return gcd(a[0], a[1]); }
static uint64_t ref_sum_to(const uint32_t *a)   { return sum_to(a[0]); }
static uint64_t ref_popcount(const uint32_t *a) { return popcount(a[0]); }
static uint64_t ref_collatz(const uint32_t *a)  { return collatz(a[0]); }
static uint64_t ref_add64(const uint32_t *a)    { return add64(lo_hi(a[0], a[1]), lo_hi(a[2], a[3])); }
static uint64_t ref_sub64(const uint32_t *a)    { return sub64(lo_hi(a[0], a[1]), lo_hi(a[2], a[3])); }
static uint64_t ref_sat_add(const uint32_t *a)  { return (uint32_t)sat_add((int32_t)a[0], (int32_t)a[1]); }
static uint64_t ref_bits(const uint32_t *a)     { return bits(a[0]); }
static uint64_t ref_max3(const uint32_t *a)     { return (uint32_t)max3((int32_t)a[0], (int32_t)a[1], (int32_t)a[2]); }
static uint64_t ref_gcd_sum(const uint32_t *a)  { return gcd_sum(a[0], a[1], a[2]); }

typedef struct {
    const char *name;
    int         nargs;  /* 32-bit argument registers used (r0..r3) */
    uint32_t    mask;   /* applied to every argument, to bound loop counts */
    int         wide;   /* 64-bit result in r0:r1 */
    RefFn       ref;
} Test;

static const Test tests[] = {
    { "smax",     2, 0xFFFFFFFFu, 0, ref_smax },
    { "umin",     2, 0xFFFFFFFFu, 0, ref_umin },
    { "iabs",     1, 0xFFFFFFFFu, 0, ref_iabs },
    { "clamp",    3, 0xFFFFFFFFu, 0, ref_clamp },
    { "gcd",      2, 0x0000FFFFu, 0, ref_gcd },      /* subtraction gcd: O(max/min) */
    { "sum_to",   1, 0x0000FFFFu, 0, ref_sum_to },   /* loops n times */
    { "popcount", 1, 0xFFFFFFFFu, 0, ref_popcount },
    { "collatz",  1, 0xFFFFFFFFu, 0, ref_collatz },
    { "add64",    4, 0xFFFFFFFFu, 1, ref_add64 },
    { "sub64",    4, 0xFFFFFFFFu, 1, ref_sub64 },
    { "sat_add",  2, 0xFFFFFFFFu, 0, ref_sat_add },
    { "bits",     1, 0xFFFFFFFFu, 0, ref_bits },
    { "max3",     3, 0xFFFFFFFFu, 0, ref_max3 },
    { "gcd_sum",  3, 0x0000FFFFu, 0, ref_gcd_sum },
};

static const uint32_t edges[] = {
    0, 1, 2, 3, 7, 1000, 12345, 0x10000,
    0x7FFFFFFF, 0x80000000, 0x80000001, 0xFFFFFFFE, 0xFFFFFFFF,
};
#define NEDGES (sizeof edges / sizeof edges[0])
#define NRANDOM 1000

static uint32_t rng_state = 0x12345678u;
static uint32_t xorshift32(void)
{
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rng_state = x;
}

/* Run one call; return 1 if it matched the reference. */
static int run_one(const Test *t, GuestFn fn, uint8_t *mem, const uint32_t *args, int *printed)
{
    Ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    for (int r = 0; r < 16; r++)
        ctx.r[r] = 0xDEAD0000u | (uint32_t)r;
    for (int i = 0; i < t->nargs; i++)
        ctx.r[i] = args[i];
    ctx.r[13] = STACK_TOP;
    ctx.r[14] = RETURN_ADDR;
    ctx.n = ctx.z = ctx.c = ctx.v = 1;      /* stale flags must not matter */
    ctx.mem = mem;

    Ctx before = ctx;
    fn(&ctx);

    uint64_t want = t->ref(args);
    uint64_t got = ctx.r[0];
    if (t->wide)
        got |= (uint64_t)ctx.r[1] << 32;

    int ok = got == want;
    for (int r = 4; r <= 13; r++)
        if (r != 12 && ctx.r[r] != before.r[r])
            ok = 0;

    if (!ok && (*printed)++ < 5) {
        printf("FAIL %s(", t->name);
        for (int i = 0; i < t->nargs; i++)
            printf("%s0x%08" PRIx32, i ? ", " : "", args[i]);
        printf(") = 0x%" PRIx64 " (want 0x%" PRIx64 ")", got, want);
        for (int r = 4; r <= 13; r++)
            if (r != 12 && ctx.r[r] != before.r[r])
                printf(" r%d clobbered", r);
        printf("\n");
    }
    return ok;
}

int main(void)
{
    uint8_t *mem = guest_mem_alloc();
    int failed_tests = 0;

    for (size_t ti = 0; ti < sizeof tests / sizeof tests[0]; ti++) {
        const Test *t = &tests[ti];
        GuestFn fn = recomp_lookup(t->name);
        if (!fn) {
            printf("FAIL %s: not found in generated code\n", t->name);
            failed_tests++;
            continue;
        }

        int runs = 0, fails = 0, printed = 0;
        uint32_t args[4];

        /* Every combination of edge values... */
        size_t combos = 1;
        for (int i = 0; i < t->nargs; i++)
            combos *= NEDGES;
        for (size_t k = 0; k < combos; k++) {
            size_t rest = k;
            for (int i = 0; i < t->nargs; i++) {
                args[i] = edges[rest % NEDGES] & t->mask;
                rest /= NEDGES;
            }
            fails += !run_one(t, fn, mem, args, &printed);
            runs++;
        }
        /* ...plus pseudo-random inputs. */
        for (int k = 0; k < NRANDOM; k++) {
            for (int i = 0; i < t->nargs; i++)
                args[i] = xorshift32() & t->mask;
            fails += !run_one(t, fn, mem, args, &printed);
            runs++;
        }

        printf("%s %-9s %d/%d\n", fails ? "FAIL" : "PASS", t->name, runs - fails, runs);
        failed_tests += fails != 0;
    }

    guest_mem_free(mem);
    if (failed_tests) {
        printf("%d function(s) failed\n", failed_tests);
        return 1;
    }
    printf("all %zu functions match the native reference\n", sizeof tests / sizeof tests[0]);
    return 0;
}
