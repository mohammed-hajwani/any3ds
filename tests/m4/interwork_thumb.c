/* Thumb side of the interworking test (compiled with -mthumb). */
#include "interwork.h"

NOINLINE uint32_t t_square(uint32_t x) { return x * x; }

/* Thumb -> ARM */
NOINLINE uint32_t t_calls_a(uint32_t x) { return a_double(x) + t_square(x + 1); }

NOINLINE uint32_t t_deep(uint32_t x, uint32_t n) { return n ? a_deep(x ^ (x >> 3), n - 1) : x; }

/* Thumb calling both an ARM function and a Thumb function that calls ARM. */
NOINLINE uint32_t t_sum_both(uint32_t x, uint32_t y) { return a_calls_t(x) + t_calls_a(y); }
