/* Real-code harness: recompiled real.c (ARM or Thumb, -O2, with libgcc)
 * vs the same C compiled natively. Arguments are 32-bit words ('w') or
 * 64-bit values ('q', in an even/odd register pair, per the AAPCS). */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "context.h"
#include "real.c"

typedef uint64_t (*Ref)(const uint64_t *a);
#define W(i) ((uint32_t)a[i])
static uint64_t r_mul64(const uint64_t *a)   { return mul64(a[0], a[1]); }
static uint64_t r_umul32(const uint64_t *a)  { return umul32(W(0), W(1)); }
static uint64_t r_smul32(const uint64_t *a)  { return (uint64_t)smul32((int32_t)W(0), (int32_t)W(1)); }
static uint64_t r_umac(const uint64_t *a)    { return umac(a[0], W(1), W(2)); }
static uint64_t r_udiv(const uint64_t *a)    { return udiv(W(0), W(1)); }
static uint64_t r_sdiv(const uint64_t *a)    { return (uint32_t)sdiv((int32_t)W(0), (int32_t)W(1)); }
static uint64_t r_umod(const uint64_t *a)    { return umod(W(0), W(1)); }
static uint64_t r_udiv64(const uint64_t *a)  { return udiv64(a[0], a[1]); }
static uint64_t r_clz32(const uint64_t *a)   { return clz32(W(0)); }
static uint64_t r_bswap(const uint64_t *a)   { return bswap(W(0)); }
static uint64_t r_bswap16(const uint64_t *a) { return bswap16(W(0)); }
static uint64_t r_sext16(const uint64_t *a)  { return (uint32_t)sext16((int32_t)W(0)); }
static uint64_t r_sext8(const uint64_t *a)   { return (uint32_t)sext8((int32_t)W(0)); }
static uint64_t r_zext16(const uint64_t *a)  { return zext16(W(0)); }
static uint64_t r_fib(const uint64_t *a)     { return fib(W(0)); }
static uint64_t r_tail(const uint64_t *a)    { return tail_caller(W(0)); }
static uint64_t r_sort(const uint64_t *a)    { return test_sort(W(0)); }
static uint64_t r_shapes(const uint64_t *a)  { return (uint32_t)test_shapes(W(0)); }
static uint64_t r_ops(const uint64_t *a)     { return test_ops(W(0), W(1)); }
static uint64_t r_fmt(const uint64_t *a)     { return test_fmt(W(0), W(1)); }

typedef struct { const char *name; const char *sig; Ref ref; uint32_t mask0; } Test;

static const Test tests[] = {
    { "mul64", "qq>q", r_mul64, 0 },     { "umul32", "ww>q", r_umul32, 0 }, { "smul32", "ww>q", r_smul32, 0 },
    { "umac", "qww>q", r_umac, 0 },      { "udiv", "ww>w", r_udiv, 0 },     { "sdiv", "ww>w", r_sdiv, 0 },
    { "umod", "ww>w", r_umod, 0 },       { "udiv64", "qq>q", r_udiv64, 0 }, { "clz32", "w>w", r_clz32, 0 },
    { "bswap", "w>w", r_bswap, 0 },      { "bswap16", "w>w", r_bswap16, 0 }, { "sext16", "w>w", r_sext16, 0 },
    { "sext8", "w>w", r_sext8, 0 },      { "zext16", "w>w", r_zext16, 0 },  { "fib", "w>w", r_fib, 15 },
    { "tail_caller", "w>w", r_tail, 0 }, { "test_sort", "w>w", r_sort, 0 }, { "test_shapes", "w>w", r_shapes, 0 },
    { "test_ops", "ww>w", r_ops, 0 },    { "test_fmt", "ww>w", r_fmt, 0 },
};

static uint32_t rng_state = 0x0DDBA11u;
static uint32_t rnd(void)
{
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rng_state = x;
}

static uint32_t word(void)
{
    static const uint32_t edges[] = { 0, 1, 2, 3, 7, 10, 16, 255, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFFFFFE };
    return rnd() % 3 ? rnd() >> (rnd() % 32) : edges[rnd() % (sizeof edges / sizeof edges[0])];
}

int main(void)
{
    uint8_t *mem = guest_mem_alloc();
    recomp_load_image(mem);
    int failed = 0;

    for (size_t ti = 0; ti < sizeof tests / sizeof tests[0]; ti++) {
        const Test *t = &tests[ti];
        GuestFn fn = recomp_lookup(t->name);
        if (!fn) { printf("FAIL %s: not found\n", t->name); failed++; continue; }
        const char *res = strchr(t->sig, '>') + 1;
        int nargs = (int)(res - 1 - t->sig), fails = 0, runs = 3000;

        for (int k = 0; k < runs; k++) {
            uint64_t a[4];
            Ctx ctx;
            memset(&ctx, 0, sizeof ctx);
            for (int r = 0; r < 16; r++)
                ctx.r[r] = 0xDEAD0000u | (uint32_t)r;
            int reg = 0;
            for (int i = 0; i < nargs; i++) {
                if (t->sig[i] == 'q') {
                    a[i] = (uint64_t)word() << 32 | word();
                    if (rnd() % 4 == 0) a[i] >>= 32;      /* small 64-bit values too */
                    reg = (reg + 1) & ~1;                  /* 64-bit args use an even register */
                    ctx.r[reg++] = (uint32_t)a[i];
                    ctx.r[reg++] = (uint32_t)(a[i] >> 32);
                } else {
                    a[i] = word();
                    if (i == 0 && t->mask0)
                        a[i] %= t->mask0 + 1;
                    ctx.r[reg++] = (uint32_t)a[i];
                }
            }
            ctx.r[13] = 0x10000000u;
            ctx.r[14] = 0x00ABC000u;
            ctx.mem = mem;
            Ctx before = ctx;
            fn(&ctx);

            uint64_t want = t->ref(a);
            uint64_t got = ctx.r[0] | (*res == 'q' ? (uint64_t)ctx.r[1] << 32 : 0);
            if (*res == 'w')
                want = (uint32_t)want;
            int ok = got == want;
            for (int r = 4; r <= 13; r++)
                ok &= r == 12 || ctx.r[r] == before.r[r];
            if (!ok && fails++ < 3) {
                printf("FAIL %s(", t->name);
                for (int i = 0; i < nargs; i++)
                    printf("%s0x%" PRIx64, i ? ", " : "", a[i]);
                printf(") = 0x%" PRIx64 ", want 0x%" PRIx64 "\n", got, want);
            }
        }
        printf("%s %-12s %d/%d\n", fails ? "FAIL" : "PASS", t->name, runs - fails, runs);
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
