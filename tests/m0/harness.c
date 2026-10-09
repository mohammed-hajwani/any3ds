/* M0 harness: run the recompiled `test` on the host and check the results.
 *
 *   int test(int a, int b) { return (a + b) * 3; }
 *
 * ARM calling convention (AAPCS): arguments arrive in r0, r1; the result
 * is returned in r0; r4–r11 and sp must survive the call.
 */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "context.h"

typedef struct { int32_t a, b; } Case;

static const Case cases[] = {
    { 2, 5 },                   /* the M0 done-condition: 21 */
    { 0, 0 },
    { -4, 1 },                  /* -9 */
    { -1, -1 },                 /* -6 */
    { INT32_MAX, 1 },           /* a + b wraps to INT32_MIN */
    { INT32_MIN, -1 },          /* a + b wraps to INT32_MAX */
    { 0x55555555, 0x55555556 }, /* (a + b) * 3 wraps */
};

/* Signed overflow is undefined in C, but the ARM code simply wraps
   modulo 2^32. Compute the expected value the way the CPU does. */
static uint32_t expected(int32_t a, int32_t b)
{
    return ((uint32_t)a + (uint32_t)b) * 3u;
}

/* Registers the AAPCS says a function must preserve. */
static int preserved_ok(const Ctx *before, const Ctx *after)
{
    int ok = 1;
    for (int r = 4; r <= 13; r++) {
        if (r == 12)    /* ip is a scratch register */
            continue;
        if (after->r[r] != before->r[r]) {
            printf("    r%d clobbered: 0x%08" PRIx32 " -> 0x%08" PRIx32 "\n",
                   r, before->r[r], after->r[r]);
            ok = 0;
        }
    }
    return ok;
}

int main(void)
{
    GuestFn test = recomp_lookup("test");
    if (!test) {
        printf("FAIL: symbol 'test' not found in generated code\n");
        return 1;
    }

    int failures = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const Case *tc = &cases[i];

        Ctx ctx;
        memset(&ctx, 0, sizeof ctx);
        for (int r = 0; r < 16; r++)
            ctx.r[r] = 0xDEAD0000u | (uint32_t)r;   /* recognisable junk */
        ctx.r[0] = (uint32_t)tc->a;
        ctx.r[1] = (uint32_t)tc->b;
        ctx.mem = NULL;                             /* M0 never touches memory */

        Ctx before = ctx;
        test(&ctx);

        uint32_t want = expected(tc->a, tc->b);
        int ok = ctx.r[0] == want;
        printf("%s test(%" PRId32 ", %" PRId32 ") = %" PRId32 " (want %" PRId32 ")\n",
               ok ? "PASS" : "FAIL", tc->a, tc->b, (int32_t)ctx.r[0], (int32_t)want);
        ok &= preserved_ok(&before, &ctx);

        if (!ok)
            failures++;
    }

    if (failures) {
        printf("%d case(s) failed\n", failures);
        return 1;
    }
    printf("all %zu cases passed\n", sizeof cases / sizeof cases[0]);
    return 0;
}
