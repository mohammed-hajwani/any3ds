/* Flag harness for flags.S: every function runs one flag-setting
 * instruction on r0/r1. Compare r0 and NZCV with a reference model.
 *
 * The model is deliberately written differently from runtime/context.h
 * (signed/unsigned 64-bit arithmetic instead of the a + ~b + 1 identity),
 * so a shared mistake is unlikely.
 */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "context.h"

typedef struct { uint32_t r, n, z, c, v; } State;

typedef enum {
    ADDS, ADCS, SUBS, SBCS, RSBS, CMP, CMN, ADDS_LSL1, ADC, SBC,
    TST, TEQ, ANDS_ROT, ANDS_SMALL, EORS_LSR1, MVNS_ASR1,
    MOVS_LSL, MOVS_LSR, MOVS_ASR, MOVS_ROR, MOVS_RRX,
} Op;

typedef struct { const char *name; Op op; unsigned amount; } Case;

static const Case cases[] = {
    { "adds_",      ADDS,       0 },
    { "adcs_",      ADCS,       0 },
    { "subs_",      SUBS,       0 },
    { "sbcs_",      SBCS,       0 },
    { "rsbs_",      RSBS,       0 },
    { "cmp_",       CMP,        0 },
    { "cmn_",       CMN,        0 },
    { "adds_lsl1",  ADDS_LSL1,  0 },
    { "adc_",       ADC,        0 },
    { "sbc_",       SBC,        0 },
    { "tst_",       TST,        0 },
    { "teq_",       TEQ,        0 },
    { "ands_rot",   ANDS_ROT,   0 },
    { "ands_small", ANDS_SMALL, 0 },
    { "eors_lsr1",  EORS_LSR1,  0 },
    { "mvns_asr1",  MVNS_ASR1,  0 },
    { "movs_lsl0",  MOVS_LSL,   0 },
    { "movs_lsl1",  MOVS_LSL,   1 },
    { "movs_lsl31", MOVS_LSL,   31 },
    { "movs_lsr1",  MOVS_LSR,   1 },
    { "movs_lsr32", MOVS_LSR,   32 },
    { "movs_asr1",  MOVS_ASR,   1 },
    { "movs_asr32", MOVS_ASR,   32 },
    { "movs_ror1",  MOVS_ROR,   1 },
    { "movs_ror31", MOVS_ROR,   31 },
    { "movs_rrx",   MOVS_RRX,   0 },
};

static const uint32_t values[] = {
    0, 1, 2, 0x7FFFFFFE, 0x7FFFFFFF, 0x80000000, 0x80000001,
    0xFFFFFFFE, 0xFFFFFFFF, 0x12345678, 0xF0F0F0F0,
};
#define NVALUES (sizeof values / sizeof values[0])

/* ---- Reference model ---- */

/* a + b + cin, flags from exact unsigned and signed sums. */
static State ref_add(State s, uint32_t a, uint32_t b, uint32_t cin)
{
    uint64_t u = (uint64_t)a + b + cin;
    int64_t  i = (int64_t)(int32_t)a + (int32_t)b + cin;
    s.r = (uint32_t)u;
    s.n = s.r >> 31;
    s.z = s.r == 0;
    s.c = u > 0xFFFFFFFFu;
    s.v = i < INT32_MIN || i > INT32_MAX;
    return s;
}

/* a - b - borrow, where borrow = !cin. ARM's C means "no borrow happened". */
static State ref_sub(State s, uint32_t a, uint32_t b, uint32_t cin)
{
    uint32_t borrow = !cin;
    int64_t  i = (int64_t)(int32_t)a - (int32_t)b - borrow;
    s.r = a - b - borrow;
    s.n = s.r >> 31;
    s.z = s.r == 0;
    s.c = (uint64_t)a >= (uint64_t)b + borrow;
    s.v = i < INT32_MIN || i > INT32_MAX;
    return s;
}

/* Barrel shifter: value and carry-out (cin when the shifter doesn't touch C). */
typedef struct { uint32_t value, carry; } Shifted;

static Shifted ref_shift(Op op, unsigned n, uint32_t x, uint32_t cin)
{
    uint64_t sx = (uint64_t)x | ((x & 0x80000000u) ? 0xFFFFFFFF00000000ull : 0);
    Shifted s;
    switch (op) {
    case MOVS_LSL:
        if (n == 0) { s.value = x; s.carry = cin; break; }
        s.value = (uint32_t)((uint64_t)x << n);
        s.carry = (uint32_t)(((uint64_t)x << n) >> 32) & 1;
        break;
    case MOVS_LSR:
        s.value = (uint32_t)((uint64_t)x >> n);
        s.carry = (uint32_t)((uint64_t)x >> (n - 1)) & 1;
        break;
    case MOVS_ASR:
        s.value = (uint32_t)(sx >> n);
        s.carry = (uint32_t)(sx >> (n - 1)) & 1;
        break;
    case MOVS_ROR: {
        uint64_t doubled = ((uint64_t)x << 32) | x;
        s.value = (uint32_t)(doubled >> n);
        s.carry = s.value >> 31;
        break;
    }
    case MOVS_RRX:
        s.value = (cin << 31) | (x >> 1);
        s.carry = x & 1;
        break;
    default:
        s.value = 0; s.carry = 0;
    }
    return s;
}

static State logical(State s, uint32_t result, uint32_t carry)
{
    s.r = result;
    s.n = result >> 31;
    s.z = result == 0;
    s.c = carry;            /* V is left alone */
    return s;
}

static State reference(const Case *tc, State in, uint32_t a, uint32_t b)
{
    State s = in;
    s.r = a;                /* r0 unchanged unless the op writes it */
    Shifted sh;
    switch (tc->op) {
    case ADDS:       return ref_add(in, a, b, 0);
    case ADCS:       return ref_add(in, a, b, in.c);
    case SUBS:       return ref_sub(in, a, b, 1);
    case SBCS:       return ref_sub(in, a, b, in.c);
    case RSBS:       return ref_sub(in, b, a, 1);
    case CMP:        s = ref_sub(in, a, b, 1); s.r = a; return s;
    case CMN:        s = ref_add(in, a, b, 0); s.r = a; return s;
    case ADDS_LSL1:  return ref_add(in, a, b << 1, 0);
    case ADC:        s.r = ref_add(in, a, b, in.c).r; return s;     /* flags untouched */
    case SBC:        s.r = ref_sub(in, a, b, in.c).r; return s;
    case TST:        s = logical(in, a & b, in.c); s.r = a; return s;
    case TEQ:        s = logical(in, a ^ b, in.c); s.r = a; return s;
    case ANDS_ROT:   return logical(in, a & 0xFF000000u, 1);
    case ANDS_SMALL: return logical(in, a & 0xFFu, in.c);
    case EORS_LSR1:  sh = ref_shift(MOVS_LSR, 1, b, in.c); return logical(in, a ^ sh.value, sh.carry);
    case MVNS_ASR1:  sh = ref_shift(MOVS_ASR, 1, b, in.c); return logical(in, ~sh.value, sh.carry);
    default:         sh = ref_shift(tc->op, tc->amount, b, in.c); return logical(in, sh.value, sh.carry);
    }
}

/* ---- Condition codes, straight from the ARM ARM table ---- */

typedef struct { const char *name; int (*holds)(uint32_t n, uint32_t z, uint32_t c, uint32_t v); } Cond;

static int eq(uint32_t n, uint32_t z, uint32_t c, uint32_t v) { (void)n; (void)c; (void)v; return z; }
static int ne(uint32_t n, uint32_t z, uint32_t c, uint32_t v) { (void)n; (void)c; (void)v; return !z; }
static int cs(uint32_t n, uint32_t z, uint32_t c, uint32_t v) { (void)n; (void)z; (void)v; return c; }
static int cc(uint32_t n, uint32_t z, uint32_t c, uint32_t v) { (void)n; (void)z; (void)v; return !c; }
static int mi(uint32_t n, uint32_t z, uint32_t c, uint32_t v) { (void)z; (void)c; (void)v; return n; }
static int pl(uint32_t n, uint32_t z, uint32_t c, uint32_t v) { (void)z; (void)c; (void)v; return !n; }
static int vs(uint32_t n, uint32_t z, uint32_t c, uint32_t v) { (void)n; (void)z; (void)c; return v; }
static int vc(uint32_t n, uint32_t z, uint32_t c, uint32_t v) { (void)n; (void)z; (void)c; return !v; }
static int hi(uint32_t n, uint32_t z, uint32_t c, uint32_t v) { (void)n; (void)v; return c && !z; }
static int ls(uint32_t n, uint32_t z, uint32_t c, uint32_t v) { (void)n; (void)v; return !c || z; }
static int ge(uint32_t n, uint32_t z, uint32_t c, uint32_t v) { (void)z; (void)c; return n == v; }
static int lt(uint32_t n, uint32_t z, uint32_t c, uint32_t v) { (void)z; (void)c; return n != v; }
static int gt(uint32_t n, uint32_t z, uint32_t c, uint32_t v) { (void)c; return !z && n == v; }
static int le(uint32_t n, uint32_t z, uint32_t c, uint32_t v) { (void)c; return z || n != v; }

static const Cond conds[] = {
    { "c_eq", eq }, { "c_ne", ne }, { "c_cs", cs }, { "c_cc", cc },
    { "c_mi", mi }, { "c_pl", pl }, { "c_vs", vs }, { "c_vc", vc },
    { "c_hi", hi }, { "c_ls", ls }, { "c_ge", ge }, { "c_lt", lt },
    { "c_gt", gt }, { "c_le", le },
};

static Ctx fresh_ctx(uint8_t *mem, State in, uint32_t a, uint32_t b)
{
    Ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.r[0] = a;
    ctx.r[1] = b;
    ctx.r[13] = 0x10000000u;
    ctx.r[14] = 0x00ABC000u;
    ctx.n = in.n; ctx.z = in.z; ctx.c = in.c; ctx.v = in.v;
    ctx.mem = mem;
    return ctx;
}

int main(void)
{
    uint8_t *mem = guest_mem_alloc();
    int failed = 0, checks = 0;

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const Case *tc = &cases[i];
        GuestFn fn = recomp_lookup(tc->name);
        if (!fn) { printf("FAIL %s: not found\n", tc->name); failed++; continue; }

        int fails = 0;
        for (size_t ai = 0; ai < NVALUES; ai++)
        for (size_t bi = 0; bi < NVALUES; bi++)
        for (uint32_t f = 0; f < 16; f++) {
            State in = { 0, (f >> 3) & 1, (f >> 2) & 1, (f >> 1) & 1, f & 1 };
            uint32_t a = values[ai], b = values[bi];
            Ctx ctx = fresh_ctx(mem, in, a, b);
            fn(&ctx);
            checks++;

            State want = reference(tc, in, a, b);
            if (ctx.r[0] != want.r || ctx.n != want.n || ctx.z != want.z ||
                ctx.c != want.c || ctx.v != want.v) {
                if (fails++ < 3)
                    printf("FAIL %s: r0=%08" PRIx32 " r1=%08" PRIx32 " nzcv=%" PRIu32 "%" PRIu32 "%" PRIu32 "%" PRIu32
                           " -> r0=%08" PRIx32 " nzcv=%" PRIu32 "%" PRIu32 "%" PRIu32 "%" PRIu32
                           " (want r0=%08" PRIx32 " nzcv=%" PRIu32 "%" PRIu32 "%" PRIu32 "%" PRIu32 ")\n",
                           tc->name, a, b, in.n, in.z, in.c, in.v,
                           ctx.r[0], ctx.n, ctx.z, ctx.c, ctx.v,
                           want.r, want.n, want.z, want.c, want.v);
            }
        }
        printf("%s %s\n", fails ? "FAIL" : "PASS", tc->name);
        failed += fails != 0;
    }

    for (size_t i = 0; i < sizeof conds / sizeof conds[0]; i++) {
        const Cond *cd = &conds[i];
        GuestFn fn = recomp_lookup(cd->name);
        if (!fn) { printf("FAIL %s: not found\n", cd->name); failed++; continue; }

        int fails = 0;
        for (uint32_t f = 0; f < 16; f++) {
            State in = { 0, (f >> 3) & 1, (f >> 2) & 1, (f >> 1) & 1, f & 1 };
            Ctx ctx = fresh_ctx(mem, in, 0xFFFFFFFFu, 0);
            fn(&ctx);
            checks++;
            uint32_t want = (uint32_t)cd->holds(in.n, in.z, in.c, in.v);
            if (ctx.r[0] != want) {
                printf("FAIL %s with nzcv=%" PRIu32 "%" PRIu32 "%" PRIu32 "%" PRIu32 ": got %" PRIu32 "\n",
                       cd->name, in.n, in.z, in.c, in.v, ctx.r[0]);
                fails++;
            }
        }
        printf("%s %s\n", fails ? "FAIL" : "PASS", cd->name);
        failed += fails != 0;
    }

    guest_mem_free(mem);
    if (failed) {
        printf("%d flag test(s) failed\n", failed);
        return 1;
    }
    printf("all %d flag checks passed\n", checks);
    return 0;
}
