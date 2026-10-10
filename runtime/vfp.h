/* VFPv2 floating point with ARM's exact semantics. Included by context.h.
 *
 * Ordinary IEEE arithmetic (round to nearest, no flush-to-zero) is the same
 * on the host, so the actual maths is plain C. What differs between ARM and
 * x86 is everything around NaNs and integer conversion, so all values are
 * passed as raw bits and those cases are handled explicitly:
 *
 *  - An operation with a NaN input returns one of its NaNs (FPSCR.DN = 0):
 *    a signalling NaN beats a quiet one, the first operand beats the
 *    second, and a signalling NaN comes out quieted (bit 22 / bit 51 set).
 *  - An invalid operation without NaN inputs (inf - inf, 0 * inf, sqrt(-1))
 *    returns the ARM default NaN 0x7FC00000 -- x86 would give 0xFFC00000.
 *  - Float -> integer conversion truncates and saturates; NaN gives 0.
 *    (The same cast in C is undefined behaviour for out-of-range values.)
 *  - vneg/vabs only flip/clear the sign bit, even for NaNs.
 *
 * Two FPSCR mode bits are honoured, because libctru turns both on for
 * every thread ("RunFast", FPSCR = 0x03000000):
 *  - FZ (flush-to-zero): denormal inputs count as zero, and denormal
 *    results become zero (sign kept).
 *  - DN (default NaN): every NaN result is the default NaN.
 *
 * Not modelled (vfp_set_fpscr stops if a program asks for them): rounding
 * modes other than round-to-nearest, short vectors (LEN/STRIDE), trapped
 * exceptions. The cumulative exception flags are not tracked.
 */
#ifndef RECOMP_VFP_H
#define RECOMP_VFP_H

#include <stdint.h>
#include <string.h>

#define FPSCR_FZ (1u << 24)
#define FPSCR_DN (1u << 25)

/* ---- single precision (raw bits in uint32_t) ---- */

static inline float    f32(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline uint32_t f32_bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline int f32_is_nan(uint32_t u)  { return (u & 0x7F800000u) == 0x7F800000u && (u & 0x007FFFFFu); }
static inline int f32_is_snan(uint32_t u) { return f32_is_nan(u) && !(u & 0x00400000u); }
#define F32_DEFAULT_NAN 0x7FC00000u
#define F32_QUIET       0x00400000u

static inline uint32_t f32_pick_nan(const Ctx *ctx, uint32_t a, uint32_t b)
{
    if (ctx->fpscr & FPSCR_DN) return F32_DEFAULT_NAN;
    if (f32_is_snan(a)) return a | F32_QUIET;
    if (f32_is_snan(b)) return b | F32_QUIET;
    return f32_is_nan(a) ? a : b;
}

/* FZ: a denormal (exponent 0, fraction non-zero) becomes a signed zero. */
static inline uint32_t f32_flush(const Ctx *ctx, uint32_t a)
{
    return (ctx->fpscr & FPSCR_FZ) && !(a & 0x7F800000u) ? a & 0x80000000u : a;
}

static inline uint32_t f32_result(const Ctx *ctx, uint32_t r)
{
    return f32_is_nan(r) ? F32_DEFAULT_NAN : f32_flush(ctx, r);
}

#define VFP_OP32(name, op)                                                      \
    static inline uint32_t vfp_##name##32(const Ctx *ctx, uint32_t a, uint32_t b) \
    {                                                                           \
        if (f32_is_nan(a) || f32_is_nan(b)) return f32_pick_nan(ctx, a, b);     \
        a = f32_flush(ctx, a);                                                  \
        b = f32_flush(ctx, b);                                                  \
        return f32_result(ctx, f32_bits(f32(a) op f32(b)));                     \
    }
VFP_OP32(add, +)
VFP_OP32(sub, -)
VFP_OP32(mul, *)
VFP_OP32(div, /)

static inline uint32_t vfp_sqrt32(const Ctx *ctx, uint32_t a)
{
    if (f32_is_nan(a)) return f32_pick_nan(ctx, a, a);
    return f32_result(ctx, f32_bits(__builtin_sqrtf(f32(f32_flush(ctx, a)))));
}

static inline uint32_t vfp_neg32(uint32_t a) { return a ^ 0x80000000u; }
static inline uint32_t vfp_abs32(uint32_t a) { return a & 0x7FFFFFFFu; }

/* ---- double precision (raw bits in uint64_t) ---- */

static inline double   f64(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
static inline uint64_t f64_bits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static inline int f64_is_nan(uint64_t u)
{
    return (u & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && (u & 0x000FFFFFFFFFFFFFull);
}
static inline int f64_is_snan(uint64_t u) { return f64_is_nan(u) && !(u & 0x0008000000000000ull); }
#define F64_DEFAULT_NAN 0x7FF8000000000000ull
#define F64_QUIET       0x0008000000000000ull

static inline uint64_t f64_pick_nan(const Ctx *ctx, uint64_t a, uint64_t b)
{
    if (ctx->fpscr & FPSCR_DN) return F64_DEFAULT_NAN;
    if (f64_is_snan(a)) return a | F64_QUIET;
    if (f64_is_snan(b)) return b | F64_QUIET;
    return f64_is_nan(a) ? a : b;
}

static inline uint64_t f64_flush(const Ctx *ctx, uint64_t a)
{
    return (ctx->fpscr & FPSCR_FZ) && !(a & 0x7FF0000000000000ull) ? a & 0x8000000000000000ull : a;
}

static inline uint64_t f64_result(const Ctx *ctx, uint64_t r)
{
    return f64_is_nan(r) ? F64_DEFAULT_NAN : f64_flush(ctx, r);
}

#define VFP_OP64(name, op)                                                      \
    static inline uint64_t vfp_##name##64(const Ctx *ctx, uint64_t a, uint64_t b) \
    {                                                                           \
        if (f64_is_nan(a) || f64_is_nan(b)) return f64_pick_nan(ctx, a, b);     \
        a = f64_flush(ctx, a);                                                  \
        b = f64_flush(ctx, b);                                                  \
        return f64_result(ctx, f64_bits(f64(a) op f64(b)));                     \
    }
VFP_OP64(add, +)
VFP_OP64(sub, -)
VFP_OP64(mul, *)
VFP_OP64(div, /)

static inline uint64_t vfp_sqrt64(const Ctx *ctx, uint64_t a)
{
    if (f64_is_nan(a)) return f64_pick_nan(ctx, a, a);
    return f64_result(ctx, f64_bits(__builtin_sqrt(f64(f64_flush(ctx, a)))));
}

static inline uint64_t vfp_neg64(uint64_t a) { return a ^ 0x8000000000000000ull; }
static inline uint64_t vfp_abs64(uint64_t a) { return a & 0x7FFFFFFFFFFFFFFFull; }

/* ---- compares: set FPSCR's NZCV; `vmrs APSR_nzcv, fpscr` copies them ----
   less: N   equal: ZC   greater: C   unordered (a NaN): CV */

static inline void vfp_set_nzcv(Ctx *ctx, uint32_t nzcv)
{
    ctx->fpscr = (ctx->fpscr & 0x0FFFFFFFu) | nzcv << 28;
}

static inline void vfp_cmp32(Ctx *ctx, uint32_t a, uint32_t b)
{
    if (f32_is_nan(a) || f32_is_nan(b)) { vfp_set_nzcv(ctx, 0x3); return; }
    float x = f32(f32_flush(ctx, a)), y = f32(f32_flush(ctx, b));
    vfp_set_nzcv(ctx, x == y ? 0x6 : x < y ? 0x8 : 0x2);
}

static inline void vfp_cmp64(Ctx *ctx, uint64_t a, uint64_t b)
{
    if (f64_is_nan(a) || f64_is_nan(b)) { vfp_set_nzcv(ctx, 0x3); return; }
    double x = f64(f64_flush(ctx, a)), y = f64(f64_flush(ctx, b));
    vfp_set_nzcv(ctx, x == y ? 0x6 : x < y ? 0x8 : 0x2);
}

/* vmsr fpscr, rX. Keeps NZCV, FZ and DN; stops on modes we don't model. */
void recomp_bad_fpscr(Ctx *ctx, uint32_t value) __attribute__((noreturn));

static inline void vfp_set_fpscr(Ctx *ctx, uint32_t value)
{
    if (value & 0x00F79F00u)        /* RMode, STRIDE, LEN, trap enables */
        recomp_bad_fpscr(ctx, value);
    ctx->fpscr = value & (0xF0000000u | FPSCR_FZ | FPSCR_DN);
}

static inline void vfp_flags_to_cpu(Ctx *ctx)
{
    ctx->n = ctx->fpscr >> 31;
    ctx->z = (ctx->fpscr >> 30) & 1;
    ctx->c = (ctx->fpscr >> 29) & 1;
    ctx->v = (ctx->fpscr >> 28) & 1;
}

/* ---- conversions ---- */

static inline uint64_t vfp_f32_to_f64(const Ctx *ctx, uint32_t a)
{
    if (f32_is_nan(a)) {    /* keep sign and payload, make it quiet */
        if (ctx->fpscr & FPSCR_DN) return F64_DEFAULT_NAN;
        return (uint64_t)(a >> 31) << 63 | 0x7FF8000000000000ull | (uint64_t)(a & 0x003FFFFFu) << 29;
    }
    return f64_bits((double)f32(f32_flush(ctx, a)));
}

static inline uint32_t vfp_f64_to_f32(const Ctx *ctx, uint64_t a)
{
    if (f64_is_nan(a)) {
        if (ctx->fpscr & FPSCR_DN) return F32_DEFAULT_NAN;
        return (uint32_t)(a >> 63) << 31 | 0x7FC00000u | (uint32_t)((a >> 29) & 0x003FFFFFu);
    }
    return f32_flush(ctx, f32_bits((float)f64(f64_flush(ctx, a))));
}

/* Truncate toward zero, saturate, NaN -> 0. Doubles hold every f32 and
   every int32/uint32 exactly, so one pair of helpers serves both widths. */
static inline uint32_t vfp_to_s32(double x)
{
    if (x != x) return 0;
    if (x >= 2147483648.0) return 0x7FFFFFFFu;
    if (x <= -2147483649.0) return 0x80000000u;
    return (uint32_t)(int32_t)x;
}

static inline uint32_t vfp_to_u32(double x)
{
    if (x != x) return 0;
    if (x >= 4294967296.0) return 0xFFFFFFFFu;
    if (x < 1.0) return 0;      /* negatives saturate; (-1, 1) truncates to 0 */
    return (uint32_t)x;
}

#endif
