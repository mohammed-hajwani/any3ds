/* Barrel-shifter harness: every function in shifts.S computes
 *   r0 = r0 + shift(r1)
 * Check each against a reference written with 64-bit arithmetic, so it
 * never relies on the same C shift tricks the translator uses.
 */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "context.h"

typedef enum { NONE, IMM, LSL, LSR, ASR, ROR, RRX } Kind;

typedef struct {
    const char *name;
    Kind        kind;
    unsigned    amount;     /* shift amount, or unused */
} Case;

static const Case cases[] = {
    { "add_reg", NONE, 0 },
    { "add_imm", IMM,  0 },
    { "lsl_1",   LSL,  1 },
    { "lsl_31",  LSL,  31 },
    { "lsr_1",   LSR,  1 },
    { "lsr_32",  LSR,  32 },
    { "asr_1",   ASR,  1 },
    { "asr_31",  ASR,  31 },
    { "asr_32",  ASR,  32 },
    { "ror_1",   ROR,  1 },
    { "ror_31",  ROR,  31 },
    { "rrx_",    RRX,  0 },
};

static const uint32_t r0_values[] = { 0, 0x11111111u };
static const uint32_t r1_values[] = {
    0, 1, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0x12345678u, 0xDEADBEEFu,
};

/* Reference implementation of operand 2. */
static uint32_t ref_operand(Kind kind, unsigned n, uint32_t x, uint32_t carry)
{
    switch (kind) {
    case NONE: return x;
    case IMM:  return 0xFF000000u;          /* see add_imm in shifts.S */
    case LSL:  return (uint32_t)((uint64_t)x << n);
    case LSR:  return (uint32_t)((uint64_t)x >> n);
    case ASR: {
        /* Sign-extend to 64 bits by hand, then a plain logical shift. */
        uint64_t sx = (uint64_t)x | ((x & 0x80000000u) ? 0xFFFFFFFF00000000ull : 0);
        return (uint32_t)(sx >> n);
    }
    case ROR: {
        uint64_t doubled = ((uint64_t)x << 32) | x;
        return (uint32_t)(doubled >> n);
    }
    case RRX:  return (uint32_t)((((uint64_t)carry << 32) | x) >> 1);
    }
    return 0;
}

int main(void)
{
    int failures = 0, total = 0;

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const Case *tc = &cases[i];
        GuestFn fn = recomp_lookup(tc->name);
        if (!fn) {
            printf("FAIL: symbol '%s' not found in generated code\n", tc->name);
            failures++;
            continue;
        }

        int case_failures = 0;
        for (size_t a = 0; a < sizeof r0_values / sizeof r0_values[0]; a++)
        for (size_t b = 0; b < sizeof r1_values / sizeof r1_values[0]; b++)
        for (uint32_t carry = 0; carry <= 1; carry++) {
            Ctx ctx;
            memset(&ctx, 0, sizeof ctx);
            ctx.r[0] = r0_values[a];
            ctx.r[1] = r1_values[b];
            ctx.c = carry;

            fn(&ctx);
            total++;

            uint32_t want = r0_values[a] + ref_operand(tc->kind, tc->amount, r1_values[b], carry);
            if (ctx.r[0] != want || ctx.r[1] != r1_values[b] || ctx.c != carry) {
                printf("FAIL %s: r0=0x%08" PRIx32 " r1=0x%08" PRIx32 " c=%" PRIu32
                       " -> r0=0x%08" PRIx32 " (want 0x%08" PRIx32 ")\n",
                       tc->name, r0_values[a], r1_values[b], carry, ctx.r[0], want);
                case_failures++;
            }
        }
        printf("%s %s\n", case_failures ? "FAIL" : "PASS", tc->name);
        failures += case_failures;
    }

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all %d shift checks passed\n", total);
    return 0;
}
