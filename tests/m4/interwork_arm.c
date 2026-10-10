/* ARM side of the interworking test (compiled with -marm). */
#include "interwork.h"

NOINLINE uint32_t a_double(uint32_t x) { return x * 2 + 1; }

/* ARM -> Thumb */
NOINLINE uint32_t a_calls_t(uint32_t x) { return t_square(x) + a_double(x); }

/* ARM -> Thumb -> ARM */
NOINLINE uint32_t a_nested(uint32_t x) { return t_calls_a(x) ^ 0x55u; }

/* Mutual recursion across modes: a_deep -> t_deep -> a_deep -> ... */
NOINLINE uint32_t a_deep(uint32_t x, uint32_t n) { return n ? t_deep(x * 3 + 1, n - 1) : x; }
