/* M4: variable shifts and multiplies. Built as ARM *and* as Thumb.
 *
 * Shift amounts come from registers here, so the compiler emits
 * register-specified shifts (`lsls r0, r1` in Thumb). C makes shifting by
 * >= 32 undefined, so amounts are masked; the asm tests (thumb.S,
 * arm_regshift.S) cover the ARM rules for 32 and above.
 */
#include <stdint.h>

#define NOINLINE __attribute__((noinline))

NOINLINE uint32_t shl(uint32_t x, uint32_t n) { return x << (n & 31); }
NOINLINE uint32_t shr(uint32_t x, uint32_t n) { return x >> (n & 31); }
NOINLINE int32_t  sar(int32_t x, uint32_t n)  { return x >> (n & 31); }
NOINLINE uint32_t rotl(uint32_t x, uint32_t n) { n &= 31; return (x << n) | (x >> ((32 - n) & 31)); }

NOINLINE uint32_t extract(uint32_t x, uint32_t lo, uint32_t w)
{
    return (x >> (lo & 31)) & ((1u << (w & 31)) - 1);
}

NOINLINE uint32_t mulhash(uint32_t x) { return x * 2654435761u; }
NOINLINE uint32_t poly(uint32_t x, uint32_t a, uint32_t b) { return x * x * a + x * b + 7; }

NOINLINE uint32_t fact(uint32_t n)
{
    uint32_t r = 1;
    while (n > 1 && n < 64)
        r *= n--;
    return r;
}

NOINLINE uint32_t spread(uint32_t x)
{
    uint32_t r = 0;
    for (uint32_t i = 0; i < 16; i++)
        r |= ((x >> i) & 1) << (2 * i);
    return r;
}
