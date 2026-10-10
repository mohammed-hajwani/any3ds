/* M2 harness: differential test of recompiled mem.c against the same
 * source compiled natively (mem.c is #included as the reference).
 *
 * Pointer arguments are marshalled: input data is copied both into a
 * scratch area of guest memory and into a host buffer, each version runs
 * on its own copy, and the results are compared: return values, output
 * buffers byte for byte, and the globals (guest image vs native).
 */
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "context.h"
#include "mem.c"

#define STACK_TOP    0x10000000u
#define RETURN_ADDR  0x00ABC000u
#define SCRATCH      0x08000000u    /* guest address of the scratch area */
#define SCRATCH_SIZE 0x1000u

static uint8_t *mem;
static uint8_t host_scratch[SCRATCH_SIZE];
static const char *current;
static int test_failures, checks;

static uint32_t rng_state = 0x2545F491u;
static uint32_t rnd(void)
{
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rng_state = x;
}

static void fail(const char *fmt, ...)
{
    if (test_failures++ < 5) {
        va_list ap;
        va_start(ap, fmt);
        printf("FAIL %s: ", current);
        vprintf(fmt, ap);
        printf("\n");
        va_end(ap);
    }
}

static void expect(uint64_t got, uint64_t want, const char *what)
{
    checks++;
    if (got != want)
        fail("%s = 0x%" PRIx64 ", want 0x%" PRIx64, what, got, want);
}

/* Scratch helpers: G() is the guest address, H() the host mirror. */
static uint32_t G(uint32_t off) { return SCRATCH + off; }
static void    *H(uint32_t off) { return host_scratch + off; }
static void put(uint32_t off, const void *data, uint32_t n)
{
    memcpy(mem + G(off), data, n);
    memcpy(H(off), data, n);
}
static void put_random(uint32_t off, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        uint8_t b = (uint8_t)rnd();
        put(off + i, &b, 1);
    }
}
static void same(uint32_t off, uint32_t n, const char *what)
{
    checks++;
    for (uint32_t i = 0; i < n; i++)
        if (mem[G(off + i)] != host_scratch[off + i]) {
            fail("%s differs at +%u: guest 0x%02x, native 0x%02x",
                 what, off + i, mem[G(off + i)], host_scratch[off + i]);
            return;
        }
}

/* Globals must match after every call: guest image vs native variables. */
static void same_global(const char *name, const void *native, uint32_t n)
{
    uint32_t addr = recomp_data_lookup(name);
    checks++;
    if (!addr)
        fail("global %s not in the data symbol table", name);
    else if (memcmp(mem + addr, native, n) != 0)
        fail("global %s differs from native", name);
}
static void same_globals(void)
{
    same_global("g_counter", &g_counter, sizeof g_counter);
    same_global("g_table", g_table, sizeof g_table);
    same_global("g_buf", g_buf, sizeof g_buf);
}

/* Call a recompiled function with up to 6 word arguments (5th+ on the stack). */
typedef struct { uint32_t r0, r1; } Ret;

static Ret gcall(const char *name, int nargs, ...)
{
    GuestFn fn = recomp_lookup(name);
    if (!fn) {
        printf("FAIL %s: %s not found in generated code\n", current, name);
        exit(1);
    }

    Ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    for (int r = 0; r < 16; r++)
        ctx.r[r] = 0xDEAD0000u | (uint32_t)r;
    ctx.r[13] = STACK_TOP - 32;
    ctx.r[14] = RETURN_ADDR;
    ctx.n = ctx.z = ctx.c = ctx.v = 1;
    ctx.mem = mem;

    va_list ap;
    va_start(ap, nargs);
    for (int i = 0; i < nargs; i++) {
        uint32_t a = va_arg(ap, uint32_t);
        if (i < 4)
            ctx.r[i] = a;
        else
            write32(&ctx, ctx.r[13] + 4u * (uint32_t)(i - 4), a);
    }
    va_end(ap);

    Ctx before = ctx;
    fn(&ctx);

    for (int r = 4; r <= 13; r++)
        if (r != 12 && ctx.r[r] != before.r[r])
            fail("%s clobbered r%d", name, r);
    return (Ret){ ctx.r[0], ctx.r[1] };
}

/* ---------------------------------------------------------------- tests */

static void t_globals(void)
{
    expect(gcall("load_const", 0).r0, load_const(), "load_const()");
    for (int i = 0; i < 50; i++) {
        expect(gcall("bump", 0).r0, bump(), "bump()");
        same_globals();
    }
    for (uint32_t n = 0; n <= 20; n++)
        expect(gcall("table_sum", 1, n).r0, table_sum(n), "table_sum(n)");
    for (int i = 0; i < 100; i++) {
        uint32_t v = rnd(), n = rnd() % 80;
        gcall("fill_buf", 2, v, n);
        fill_buf(v, n);
        same_globals();
    }
    expect(gcall("msg_len", 0).r0, (uint32_t)msg_len(), "msg_len()");
}

static void random_string(uint32_t off, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        uint8_t b = (uint8_t)(1 + rnd() % 255);     /* nonzero, incl. bytes >= 0x80 */
        put(off + i, &b, 1);
    }
    uint8_t nul = 0;
    put(off + len, &nul, 1);
}

static void t_strings(void)
{
    for (int i = 0; i < 300; i++) {
        uint32_t len = rnd() % 40;
        random_string(0, len);
        expect(gcall("my_strlen", 1, G(0)).r0, (uint32_t)my_strlen(H(0)), "my_strlen(s)");

        /* b = a, then maybe changed at one position or cut short. */
        memcpy(H(0x80), H(0), len + 1);
        memcpy(mem + G(0x80), mem + G(0), len + 1);
        if (len && rnd() % 3) {
            uint32_t pos = rnd() % len;
            uint8_t b = (uint8_t)(rnd() % 3 ? 1 + rnd() % 255 : 0);
            put(0x80 + pos, &b, 1);
        }
        expect(gcall("my_strcmp", 2, G(0), G(0x80)).r0,
               (uint32_t)my_strcmp(H(0), H(0x80)), "my_strcmp(a, b)");
    }
}

static void t_memcpy(void)
{
    for (int i = 0; i < 300; i++) {
        put_random(0x100, 0x100);
        uint32_t d = 0x100 + rnd() % 64, s = 0x100 + rnd() % 64, n = rnd() % 64;  /* may overlap */
        gcall("my_memcpy", 3, G(d), G(s), n);
        my_memcpy(H(d), H(s), n);
        same(0x100, 0x100, "memory after my_memcpy");
    }
}

static void t_struct(void)
{
    for (int i = 0; i < 300; i++) {
        put_random(0, sizeof(Rec));
        expect(gcall("rec_mix", 1, G(0)).r0, (uint32_t)rec_mix(H(0)), "rec_mix(r)");

        uint32_t x = rnd();
        gcall("rec_set", 2, G(0x40), x);
        rec_set(H(0x40), (int32_t)x);
        same(0x40, sizeof(Rec), "Rec after rec_set");

        put_random(0x80, sizeof(Big));
        put_random(0xC0, sizeof(Big));
        gcall("big_copy", 2, G(0xC0), G(0x80));
        big_copy(H(0xC0), H(0x80));
        same(0x80, 2 * sizeof(Big) + 0x10, "memory after big_copy");
    }
}

static void t_lists(void)
{
    for (int i = 0; i < 200; i++) {
        /* Same values, two layouts: guest nodes are {u32 next; u32 val}. */
        uint32_t k = rnd() % 9;
        Node host_nodes[8];
        for (uint32_t j = 0; j < k; j++) {
            uint32_t val = rnd();
            uint32_t next = j + 1 < k ? G(0x200 + 8 * (j + 1)) : 0;
            memcpy(mem + G(0x200 + 8 * j), &next, 4);
            memcpy(mem + G(0x204 + 8 * j), &val, 4);
            host_nodes[j].val = val;
            host_nodes[j].next = j + 1 < k ? &host_nodes[j + 1] : NULL;
        }
        expect(gcall("list_sum", 1, k ? G(0x200) : 0).r0,
               list_sum(k ? &host_nodes[0] : NULL), "list_sum(list)");
    }
    for (uint32_t start = 0; start < 8; start++)
        expect(gcall("global_list", 1, start).r0, global_list(start), "global_list(start)");
}

static void t_stack(void)
{
    for (int i = 0; i < 300; i++) {
        uint32_t a = rnd(), b = rnd(), c = rnd(), d = rnd(), e = rnd(), f = rnd();
        expect(gcall("many_args", 6, a, b, c, d, e, f).r0, many_args(a, b, c, d, e, f),
               "many_args(a..f)");
        expect(gcall("call_many", 1, a).r0, call_many(a), "call_many(x)");
        expect(gcall("local_array", 1, a).r0, local_array(a), "local_array(seed)");
    }
}

static void t_bubble(void)
{
    for (int i = 0; i < 200; i++) {
        uint32_t n = rnd() % 13;
        put_random(0, 4 * 16);
        gcall("bubble", 2, G(0), n);
        bubble(H(0), n);
        same(0, 4 * 16, "array after bubble");
    }
}

static void t_64bit(void)
{
    for (int i = 0; i < 300; i++) {
        put_random(0, 32);
        Ret r = gcall("load64", 1, G(0));
        expect(r.r0 | (uint64_t)r.r1 << 32, load64(H(0)), "load64(p)");

        uint64_t v = (uint64_t)rnd() << 32 | rnd();
        if (i % 7 == 0)
            v |= 0xFFFFFFFFu;               /* force the carry into the high word */
        put_random(0x40, 32);
        /* AAPCS: a 64-bit argument after a pointer goes in r2:r3. */
        gcall("store64", 4, G(0x40), 0u, (uint32_t)v, (uint32_t)(v >> 32));
        store64(H(0x40), v);
        same(0x40, 32, "memory after store64");
    }
}

static void t_switch(void)
{
    static const uint32_t xs[] = { 0, 1, 2, 3, 4, 5, 6, 7, 100, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF };
    for (size_t i = 0; i < sizeof xs / sizeof xs[0]; i++)
        expect(gcall("sw", 1, xs[i]).r0, sw(xs[i]), "sw(x)");
}

int main(void)
{
    static const struct { const char *name; void (*run)(void); } tests[] = {
        { "globals",  t_globals },
        { "strings",  t_strings },
        { "memcpy",   t_memcpy },
        { "structs",  t_struct },
        { "lists",    t_lists },
        { "stack",    t_stack },
        { "bubble",   t_bubble },
        { "64-bit",   t_64bit },
        { "switch",   t_switch },
    };

    mem = guest_mem_alloc();
    recomp_load_image(mem);

    int failed = 0;
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        current = tests[i].name;
        test_failures = checks = 0;
        tests[i].run();
        printf("%s %-8s %d checks\n", test_failures ? "FAIL" : "PASS", current, checks);
        failed += test_failures != 0;
    }

    guest_mem_free(mem);
    if (failed) {
        printf("%d test group(s) failed\n", failed);
        return 1;
    }
    printf("all groups match the native reference\n");
    return 0;
}
