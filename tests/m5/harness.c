/* M5 harness: recompiled fp.c vs the same C compiled natively.
 *
 * Arguments are placed by the AAPCS-VFP (hard-float) rules: ints/pointers
 * in r0-r3; each float in the first free single register s0-s15; each
 * double in the first free *pair* (d0-d7), so a later float can back-fill
 * a hole left by alignment. Results come back in r0, s0 or d0.
 *
 * Results are compared bit for bit, except that any NaN equals any NaN:
 * exactly *which* NaN ARM produces is checked by the Unicorn diff.
 */
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "context.h"
#include "fp.c"

#define SCRATCH 0x08000000u

static uint8_t *mem;
static uint8_t host_scratch[0x400];

static uint32_t rng_state = 0x5EED1234u;
static uint32_t rnd(void)
{
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rng_state = x;
}

static float  f_of(uint64_t b) { float f; uint32_t u = (uint32_t)b; memcpy(&f, &u, 4); return f; }
static double d_of(uint64_t b) { double d; memcpy(&d, &b, 8); return d; }
static uint64_t bits_f(float f)  { uint32_t u; memcpy(&u, &f, 4); return u; }
static uint64_t bits_d(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }

/* ---- references: unpack raw argument bits, return raw result bits ---- */
typedef uint64_t (*Ref)(const uint64_t *a);
#define F(i) f_of(a[i])
#define D(i) d_of(a[i])
#define I(i) ((int32_t)(uint32_t)a[i])
#define U(i) ((uint32_t)a[i])
static uint64_t r_fadd(const uint64_t *a)  { return bits_f(fadd(F(0), F(1))); }
static uint64_t r_fmix(const uint64_t *a)  { return bits_f(fmix(F(0), F(1), F(2))); }
static uint64_t r_dmix(const uint64_t *a)  { return bits_d(dmix(D(0), D(1))); }
static uint64_t r_fabs(const uint64_t *a)  { return bits_f(fabs_(F(0))); }
static uint64_t r_fneg(const uint64_t *a)  { return bits_f(fneg(F(0))); }
static uint64_t r_fsqrt(const uint64_t *a) { return bits_f(fsqrt(F(0))); }
static uint64_t r_dsqrt(const uint64_t *a) { return bits_d(dsqrt(D(0))); }
static uint64_t r_f2i(const uint64_t *a)   { return (uint32_t)f2i(F(0)); }
static uint64_t r_f2u(const uint64_t *a)   { return f2u(F(0)); }
static uint64_t r_d2i(const uint64_t *a)   { return (uint32_t)d2i(D(0)); }
static uint64_t r_i2f(const uint64_t *a)   { return bits_f(i2f(I(0))); }
static uint64_t r_u2d(const uint64_t *a)   { return bits_d(u2d(U(0))); }
static uint64_t r_f2d(const uint64_t *a)   { return bits_d(f2d(F(0))); }
static uint64_t r_d2f(const uint64_t *a)   { return bits_f(d2f(D(0))); }
static uint64_t r_fless(const uint64_t *a) { return (uint32_t)fless(F(0), F(1)); }
static uint64_t r_dcmp3(const uint64_t *a) { return (uint32_t)dcmp3(D(0), D(1)); }
static uint64_t r_fequal(const uint64_t *a){ return (uint32_t)fequal(F(0), F(1)); }
static uint64_t r_poly(const uint64_t *a)  { return bits_f(poly(F(0))); }
static uint64_t r_fclamp(const uint64_t *a){ return bits_f(fclamp(F(0), F(1), F(2))); }
static uint64_t r_many(const uint64_t *a)  { return bits_d(many(D(0), F(1), D(2), F(3), I(4))); }
static uint64_t r_calls(const uint64_t *a) { return bits_f(calls(F(0))); }

/* Argument kinds: f = float, d = double, i = int. The result kind follows '>'. */
typedef struct { const char *name; const char *sig; Ref ref; int in_range; } Test;

static const Test tests[] = {
    { "fadd", "ff>f", r_fadd, 0 },     { "fmix", "fff>f", r_fmix, 0 },  { "dmix", "dd>d", r_dmix, 0 },
    { "fabs_", "f>f", r_fabs, 0 },     { "fneg", "f>f", r_fneg, 0 },    { "fsqrt", "f>f", r_fsqrt, 0 },
    { "dsqrt", "d>d", r_dsqrt, 0 },    { "f2i", "f>i", r_f2i, 1 },      { "f2u", "f>i", r_f2u, 2 },
    { "d2i", "d>i", r_d2i, 1 },        { "i2f", "i>f", r_i2f, 0 },      { "u2d", "i>d", r_u2d, 0 },
    { "f2d", "f>d", r_f2d, 0 },        { "d2f", "d>f", r_d2f, 0 },      { "fless", "ff>i", r_fless, 0 },
    { "dcmp3", "dd>i", r_dcmp3, 0 },   { "fequal", "ff>i", r_fequal, 0 }, { "poly", "f>f", r_poly, 0 },
    { "fclamp", "fff>f", r_fclamp, 0 }, { "many", "dfdfi>d", r_many, 0 }, { "calls", "f>f", r_calls, 0 },
};

static int failures, checks;
static const char *current;

/* Interesting float/double values, plus random bit patterns. */
static uint64_t pick(char kind, int in_range)
{
    static const float fs[] = { 0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 1.5f, 3.0f, -2.75f, 1e-40f, -1e-40f,
                                3.4e38f, -3.4e38f, 123456.789f, 1e10f, INFINITY, -INFINITY, NAN };
    static const double ds[] = { 0.0, -0.0, 1.0, -1.0, 0.5, 3.25, -7.125, 1e-310, 1.7e308, -1.7e308,
                                 1e100, INFINITY, -INFINITY, NAN };
    if (kind == 'i')
        return rnd() % 3 ? rnd() : (uint32_t)(int32_t)(rnd() % 200 - 100);
    if (in_range) {
        /* float->int casts only defined in range: [-2e9, 2e9] or [0, 4e9]. */
        double v = (double)(rnd() % 2000000) - (in_range == 1 ? 1000000.0 : 0.0);
        v *= (rnd() % 4 == 0) ? 1000.0 : (rnd() % 1000) / 1000.0;
        return kind == 'f' ? bits_f((float)v) : bits_d(v);
    }
    if (kind == 'f') {
        if (rnd() % 2)
            return bits_f(fs[rnd() % (sizeof fs / sizeof fs[0])]);
        return rnd();
    }
    if (rnd() % 2)
        return bits_d(ds[rnd() % (sizeof ds / sizeof ds[0])]);
    return (uint64_t)rnd() << 32 | rnd();
}

static int is_nan(char kind, uint64_t b)
{
    return kind == 'f' ? isnan(f_of(b)) : kind == 'd' ? isnan(d_of(b)) : 0;
}

static void run(const Test *t)
{
    GuestFn fn = recomp_lookup(t->name);
    if (!fn) { printf("FAIL %s: not found\n", t->name); failures++; return; }

    const char *res = strchr(t->sig, '>') + 1;
    int nargs = (int)(res - 1 - t->sig);
    for (int iter = 0; iter < 3000; iter++) {
        uint64_t a[8];
        Ctx ctx;
        memset(&ctx, 0, sizeof ctx);
        for (int r = 0; r < 16; r++)
            ctx.r[r] = 0xDEAD0000u | (uint32_t)r;
        for (int s = 0; s < 32; s++)
            ctx.vfp.u[s] = 0xBAD00000u | (uint32_t)s;
        ctx.r[13] = 0x10000000u;
        ctx.r[14] = 0x00ABC000u;
        ctx.mem = mem;

        /* AAPCS-VFP argument allocation, with back-filling. */
        int next_core = 0;
        uint32_t used = 0;      /* bitmap of s0-s15 */
        for (int i = 0; i < nargs; i++) {
            char k = t->sig[i];
            a[i] = pick(k, t->in_range);
            if (k == 'i') {
                ctx.r[next_core++] = (uint32_t)a[i];
            } else if (k == 'f') {
                int s = 0;
                while (used & (1u << s)) s++;
                used |= 1u << s;
                ctx.vfp.u[s] = (uint32_t)a[i];
            } else {
                int s = 0;
                while (used & (3u << s)) s += 2;
                used |= 3u << s;
                ctx.vfp.x[s / 2] = a[i];
            }
        }
        Ctx before = ctx;
        fn(&ctx);
        checks++;

        uint64_t want = t->ref(a);
        uint64_t got = *res == 'i' ? ctx.r[0] : *res == 'f' ? ctx.vfp.u[0] : ctx.vfp.x[0];
        int ok = got == want || (is_nan(*res, got) && is_nan(*res, want));
        for (int r = 4; r <= 13; r++)
            ok &= r == 12 || ctx.r[r] == before.r[r];
        for (int d = 8; d < 16; d++)
            ok &= ctx.vfp.x[d] == before.vfp.x[d];      /* callee-saved d8-d15 */
        if (!ok && failures++ < 8) {
            printf("FAIL %s(", t->name);
            for (int i = 0; i < nargs; i++)
                printf("%s0x%" PRIx64, i ? ", " : "", a[i]);
            printf(") = 0x%" PRIx64 ", want 0x%" PRIx64 "\n", got, want);
        }
    }
}

static void run_memory_tests(void)
{
    current = "memory";
    GuestFn g_sum = recomp_lookup("sum"), g_scale = recomp_lookup("scale"), g_dot = recomp_lookup("dot");
    for (int iter = 0; iter < 500; iter++) {
        uint32_t n = rnd() % 33;
        for (uint32_t i = 0; i < 0x200; i += 4) {          /* floats in the lower half */
            float f = (float)((int32_t)(rnd() % 20001) - 10000) / 64.0f;
            memcpy(host_scratch + i, &f, 4);
        }
        for (uint32_t i = 0x200; i < 0x400; i += 8) {      /* doubles in the upper half */
            double d = (double)((int32_t)(rnd() % 20001) - 10000) / 128.0;
            memcpy(host_scratch + i, &d, 8);
        }
        memcpy(mem + SCRATCH, host_scratch, sizeof host_scratch);
        float k = (float)(rnd() % 1000) / 10.0f - 50.0f;

        Ctx ctx;
        memset(&ctx, 0, sizeof ctx);
        ctx.r[13] = 0x10000000u; ctx.r[14] = 0x00ABC000u; ctx.mem = mem;

        ctx.r[0] = SCRATCH; ctx.r[1] = n;
        g_sum(&ctx);
        float want_sum = sum((const float *)host_scratch, n);
        checks++;
        if (ctx.vfp.u[0] != bits_f(want_sum) && failures++ < 8)
            printf("FAIL sum(n=%u) = 0x%08x, want 0x%08" PRIx64 "\n", n, ctx.vfp.u[0], bits_f(want_sum));

        ctx.r[0] = SCRATCH; ctx.r[1] = n; ctx.vfp.f[0] = k;
        g_scale(&ctx);
        scale((float *)host_scratch, n, k);
        checks++;
        if (memcmp(mem + SCRATCH, host_scratch, 4 * n) != 0 && failures++ < 8)
            printf("FAIL scale(n=%u): memory differs\n", n);

        uint32_t m = n % 16;
        ctx.r[0] = SCRATCH + 0x200; ctx.r[1] = SCRATCH + 0x280; ctx.r[2] = m;
        g_dot(&ctx);
        double want_dot = dot((const double *)(host_scratch + 0x200), (const double *)(host_scratch + 0x280), m);
        checks++;
        if (ctx.vfp.x[0] != bits_d(want_dot) && failures++ < 8)
            printf("FAIL dot(m=%u) = 0x%" PRIx64 ", want 0x%" PRIx64 "\n", m, ctx.vfp.x[0], bits_d(want_dot));
    }
}

int main(void)
{
    mem = guest_mem_alloc();
    recomp_load_image(mem);
    int failed_tests = 0;
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        current = tests[i].name;
        failures = 0;
        checks = 0;
        run(&tests[i]);
        printf("%s %-7s %d checks\n", failures ? "FAIL" : "PASS", tests[i].name, checks);
        failed_tests += failures != 0;
    }
    failures = checks = 0;
    run_memory_tests();
    printf("%s memory  %d checks (sum, scale, dot)\n", failures ? "FAIL" : "PASS", checks);
    failed_tests += failures != 0;

    guest_mem_free(mem);
    if (failed_tests) {
        printf("%d test(s) failed\n", failed_tests);
        return 1;
    }
    printf("all float functions match the native reference\n");
    return 0;
}
