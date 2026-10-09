/* M1 test functions: branches, loops, compares, flags, conditional
 * execution and calls between functions.
 *
 * Built twice: for ARM (then recompiled) and natively on the host as the
 * reference that the harness compares against. Keep it free of undefined
 * behaviour, so both builds must agree: fixed-width types only, no signed
 * overflow.
 */
#include <stdint.h>

#define NOINLINE __attribute__((noinline))

/* Conditional execution: cmp + movge / movcc / rsblt, conditional return. */
NOINLINE int32_t smax(int32_t a, int32_t b) { return a > b ? a : b; }
NOINLINE uint32_t umin(uint32_t a, uint32_t b) { return a < b ? a : b; }
NOINLINE int32_t iabs(int32_t a) { return a < 0 ? (int32_t)(0u - (uint32_t)a) : a; }
NOINLINE int32_t clamp(int32_t x, int32_t lo, int32_t hi) { return x < lo ? lo : x > hi ? hi : x; }

/* Loops and branches. */
NOINLINE uint32_t gcd(uint32_t a, uint32_t b)
{
    if (a == 0) return b;
    while (b != 0) {
        if (a > b) a -= b;
        else b -= a;
    }
    return a;
}

NOINLINE uint32_t sum_to(uint32_t n)
{
    uint32_t s = 0;
    while (n) s += n--;
    return s;
}

NOINLINE uint32_t popcount(uint32_t x)
{
    uint32_t n = 0;
    while (x) { n += x & 1; x >>= 1; }
    return n;
}

NOINLINE uint32_t collatz(uint32_t n)
{
    uint32_t steps = 0;
    while (n > 1 && steps < 1000) {
        n = (n & 1) ? n * 3 + 1 : n >> 1;
        steps++;
    }
    return steps;
}

/* 64-bit arithmetic: carry between two 32-bit halves (adds/adc, subs/sbc). */
NOINLINE uint64_t add64(uint64_t a, uint64_t b) { return a + b; }
NOINLINE uint64_t sub64(uint64_t a, uint64_t b) { return a - b; }

/* The overflow (V) flag. */
NOINLINE int32_t sat_add(int32_t a, int32_t b)
{
    int32_t r;
    if (__builtin_add_overflow(a, b, &r))
        return a < 0 ? INT32_MIN : INT32_MAX;
    return r;
}

/* Logical ops: and/orr/eor/bic/mvn/uxtb. */
NOINLINE uint32_t bits(uint32_t x)
{
    return (x & 0xff) ^ (x | 0xf0f0) ^ ~(x >> 3) ^ (x & ~0x0ff0u);
}

/* Calls between functions: bl, and push/pop of lr. */
NOINLINE int32_t max3(int32_t a, int32_t b, int32_t c) { return smax(smax(a, b), c); }
NOINLINE uint32_t gcd_sum(uint32_t a, uint32_t b, uint32_t c) { return gcd(a, b) + gcd(b, c); }
