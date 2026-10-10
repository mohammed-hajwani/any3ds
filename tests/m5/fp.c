/* M5 test functions: VFP floating point (ARM mode, -mfloat-abi=hard).
 *
 * Built for ARM (then recompiled) and natively as the reference. Float
 * and double arguments travel in VFP registers (s0.., d0..), results come
 * back in s0/d0. Casts of out-of-range floats to integers are undefined in
 * C, so the native harness only feeds those in range; the Unicorn diff
 * covers ARM's saturating behaviour.
 */
#include <stdint.h>

#define NOINLINE __attribute__((noinline))

NOINLINE float  fadd(float a, float b) { return a + b; }
NOINLINE float  fmix(float a, float b, float c) { return a * b - c / (a + 1.5f); }
NOINLINE double dmix(double a, double b) { return a * b + a / b - 3.25; }
NOINLINE float  fabs_(float a) { return __builtin_fabsf(a); }
NOINLINE float  fneg(float a) { return -a; }
NOINLINE float  fsqrt(float a) { return __builtin_sqrtf(a); }
NOINLINE double dsqrt(double a) { return __builtin_sqrt(a); }

NOINLINE int32_t  f2i(float a) { return (int32_t)a; }
NOINLINE uint32_t f2u(float a) { return (uint32_t)a; }
NOINLINE int32_t  d2i(double a) { return (int32_t)a; }
NOINLINE float    i2f(int32_t a) { return (float)a; }
NOINLINE double   u2d(uint32_t a) { return (double)a; }
NOINLINE double   f2d(float a) { return a; }
NOINLINE float    d2f(double a) { return (float)a; }

NOINLINE int fless(float a, float b) { return a < b; }
NOINLINE int dcmp3(double a, double b) { return (a > b) - (a < b); }
NOINLINE int fequal(float a, float b) { return a == b; }

NOINLINE float poly(float x) { return ((2.0f * x + 3.0f) * x - 0.5f) * x + 7.0f; }
NOINLINE float fclamp(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }

/* Mixed argument kinds: the hard-float ABI back-fills single registers. */
NOINLINE double many(double a, float b, double c, float d, int32_t e) { return a + b * c - d + e; }

/* Calls and callee-saved VFP registers (d8-d15). */
NOINLINE float calls(float a) { return fadd(a, 1.0f) * fsqrt(a) + fneg(a); }

/* Floats in memory. */
NOINLINE float sum(const float *v, uint32_t n)
{
    float s = 0;
    for (uint32_t i = 0; i < n; i++)
        s += v[i];
    return s;
}

NOINLINE void scale(float *v, uint32_t n, float k)
{
    for (uint32_t i = 0; i < n; i++)
        v[i] *= k;
}

NOINLINE double dot(const double *a, const double *b, uint32_t n)
{
    double s = 0;
    for (uint32_t i = 0; i < n; i++)
        s += a[i] * b[i];
    return s;
}
