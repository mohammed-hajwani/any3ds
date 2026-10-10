/* CPU context shared by the runtime and all generated code. */
#ifndef RECOMP_CONTEXT_H
#define RECOMP_CONTEXT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    uint32_t r[16];        // r0–r15 (r13 = sp, r14 = lr, r15 = pc)
    uint32_t n, z, c, v;   // condition flags, kept as 0/1
    uint8_t *mem;          // base of the 4 GiB guest address space

    /* VFPv2: 32 single-precision registers s0-s31, which overlap the 16
       double registers d0-d15 (d[k] = s[2k] low half, s[2k+1] high half).
       Generated code works on the raw bits (u/x) so no NaN payload is ever
       changed by the host; f/d are value views for harnesses. */
    union {
        uint32_t u[32];
        uint64_t x[16];
        float    f[32];
        double   d[16];
    } vfp;
    uint32_t fpscr;        // only the NZCV compare flags (bits 31-28) are modelled

    uint32_t ge;           // ARMv6 SIMD "greater or equal" flags (bits 3-0), for sel
    uint32_t tls;          // CP15 c13 TPIDRURO: the thread-local storage pointer
    uint32_t excl_addr;    // ldrex/strex exclusive monitor: the address ldrex marked,
    uint32_t excl_valid;   //   and whether the monitor is armed
} Ctx;

/* ---- Tracing (M3 differential testing) ----
   Code generated with `python -m recomp --trace` defines RECOMP_TRACING and
   calls RECOMP_TRACE before every instruction; every memory write is
   reported too. A test driver implements both hooks. Normal builds don't
   pay anything. */
#ifdef RECOMP_TRACING
void recomp_trace(Ctx *ctx, uint32_t addr, const char *text);
void recomp_trace_write(uint32_t addr, uint32_t size, uint64_t value);
#define RECOMP_TRACE(addr, text) recomp_trace(ctx, (addr), (text))
#define TRACE_WRITE(a, size, v)  recomp_trace_write((a), (size), (v))
#else
#define TRACE_WRITE(a, size, v)  ((void)0)
#endif

/* Guest memory accesses. memcpy makes unaligned accesses work, as on the
   3DS. Loads zero-extend; ldrsb/ldrsh sign-extend at the call site.

   Guest addresses are 32-bit, so a multi-byte access starting in the last
   bytes of the address space wraps around to address 0. Without the slow
   path, a 4-byte read at 0xFFFFFFFF would touch host memory past the end
   of the 4 GiB mapping. (Found by the M3 differential test.) */
static inline uint32_t read8(Ctx *c, uint32_t a)   { return c->mem[a]; }

static inline uint32_t read_wrapped(Ctx *c, uint32_t a, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++)
        v |= (uint32_t)c->mem[(uint32_t)(a + (uint32_t)i)] << (8 * i);
    return v;
}

static inline void write_wrapped(Ctx *c, uint32_t a, uint32_t v, int n)
{
    for (int i = 0; i < n; i++)
        c->mem[(uint32_t)(a + (uint32_t)i)] = (uint8_t)(v >> (8 * i));
}

static inline uint32_t read32(Ctx *c, uint32_t a)
{
    if (a > 0xFFFFFFFCu) return read_wrapped(c, a, 4);
    uint32_t v; memcpy(&v, c->mem + a, 4); return v;
}

static inline uint32_t read16(Ctx *c, uint32_t a)
{
    if (a == 0xFFFFFFFFu) return read_wrapped(c, a, 2);
    uint16_t v; memcpy(&v, c->mem + a, 2); return v;
}

static inline void write32(Ctx *c, uint32_t a, uint32_t v)
{
    TRACE_WRITE(a, 4, v);
    if (a > 0xFFFFFFFCu) { write_wrapped(c, a, v, 4); return; }
    memcpy(c->mem + a, &v, 4);
}

static inline void write16(Ctx *c, uint32_t a, uint32_t v)
{
    uint16_t h = (uint16_t)v;
    TRACE_WRITE(a, 2, h);
    if (a == 0xFFFFFFFFu) { write_wrapped(c, a, h, 2); return; }
    memcpy(c->mem + a, &h, 2);
}

static inline void write8(Ctx *c, uint32_t a, uint32_t v)  { TRACE_WRITE(a, 1, v & 0xFFu); c->mem[a] = (uint8_t)v; }

/* 64-bit accesses (VFP doubles): low word at the lower address. */
static inline uint64_t read64(Ctx *c, uint32_t a)
{
    return read32(c, a) | (uint64_t)read32(c, a + 4u) << 32;
}

static inline void write64(Ctx *c, uint32_t a, uint64_t v)
{
    TRACE_WRITE(a, 8, v);
    uint32_t lo = (uint32_t)v, hi = (uint32_t)(v >> 32);
    if (a > 0xFFFFFFF8u) { write_wrapped(c, a, lo, 4); write_wrapped(c, a + 4u, hi, 4); return; }
    memcpy(c->mem + a, &lo, 4);
    memcpy(c->mem + a + 4, &hi, 4);
}

/* ---- Condition codes (ARM ARM, "Condition codes" table) ---- */

static inline int cond_eq(const Ctx *c) { return c->z; }
static inline int cond_ne(const Ctx *c) { return !c->z; }
static inline int cond_cs(const Ctx *c) { return c->c; }               /* unsigned >= */
static inline int cond_cc(const Ctx *c) { return !c->c; }              /* unsigned <  */
static inline int cond_mi(const Ctx *c) { return c->n; }
static inline int cond_pl(const Ctx *c) { return !c->n; }
static inline int cond_vs(const Ctx *c) { return c->v; }
static inline int cond_vc(const Ctx *c) { return !c->v; }
static inline int cond_hi(const Ctx *c) { return c->c && !c->z; }      /* unsigned >  */
static inline int cond_ls(const Ctx *c) { return !c->c || c->z; }      /* unsigned <= */
static inline int cond_ge(const Ctx *c) { return c->n == c->v; }       /* signed >=   */
static inline int cond_lt(const Ctx *c) { return c->n != c->v; }       /* signed <    */
static inline int cond_gt(const Ctx *c) { return !c->z && c->n == c->v; }
static inline int cond_le(const Ctx *c) { return c->z || c->n != c->v; }

/* ---- Flag-setting arithmetic ---- */

/* a + b + carry_in, setting NZCV. Every ARM add/subtract reduces to this. */
static inline uint32_t addc_flags(Ctx *ctx, uint32_t a, uint32_t b, uint32_t carry_in)
{
    uint64_t wide = (uint64_t)a + b + carry_in;
    uint32_t r = (uint32_t)wide;
    ctx->n = r >> 31;
    ctx->z = r == 0;
    ctx->c = (uint32_t)(wide >> 32);        /* carry out of bit 31 */
    ctx->v = ((a ^ r) & (b ^ r)) >> 31;     /* inputs agree in sign, result doesn't */
    return r;
}

static inline uint32_t add_flags(Ctx *ctx, uint32_t a, uint32_t b) { return addc_flags(ctx, a, b, 0); }
static inline uint32_t adc_flags(Ctx *ctx, uint32_t a, uint32_t b) { return addc_flags(ctx, a, b, ctx->c); }

/* ARM computes a - b as a + ~b + 1. So after a subtraction C = 1 means
   "no borrow" (a >= b unsigned) -- the opposite of x86's carry flag. */
static inline uint32_t sub_flags(Ctx *ctx, uint32_t a, uint32_t b) { return addc_flags(ctx, a, ~b, 1); }
static inline uint32_t sbc_flags(Ctx *ctx, uint32_t a, uint32_t b) { return addc_flags(ctx, a, ~b, ctx->c); }

/* Logical ops with S: N and Z from the result. C comes from the barrel
   shifter (set separately by the generated code); V is untouched. */
static inline void set_nz(Ctx *ctx, uint32_t r) { ctx->n = r >> 31; ctx->z = r == 0; }

/* ---- Register-specified shifts ----
   `lsls r0, r1` / `add r0, r1, r2, lsl r3`: the amount comes from a
   register at runtime. Only its bottom byte counts (so 256 shifts by 0),
   and unlike C, shifting by 32 or more is well defined. An amount of 0
   leaves both the value and C alone. With set_c, C gets the last bit
   shifted out (only logical instructions with S want that). */

static inline uint32_t shift_lsl(Ctx *ctx, uint32_t x, uint32_t amount, int set_c)
{
    uint32_t s = amount & 0xFF;
    if (s == 0) return x;
    if (set_c) ctx->c = s <= 32 ? (uint32_t)(((uint64_t)x << s) >> 32) & 1 : 0;
    return s < 32 ? x << s : 0;
}

static inline uint32_t shift_lsr(Ctx *ctx, uint32_t x, uint32_t amount, int set_c)
{
    uint32_t s = amount & 0xFF;
    if (s == 0) return x;
    if (set_c) ctx->c = s <= 32 ? (x >> (s - 1)) & 1 : 0;
    return s < 32 ? x >> s : 0;
}

static inline uint32_t shift_asr(Ctx *ctx, uint32_t x, uint32_t amount, int set_c)
{
    uint32_t s = amount & 0xFF;
    if (s == 0) return x;
    uint32_t sign = x >> 31;
    if (s >= 32) {                          /* every bit becomes the sign bit */
        if (set_c) ctx->c = sign;
        return sign ? 0xFFFFFFFFu : 0;
    }
    if (set_c) ctx->c = (x >> (s - 1)) & 1;
    return (uint32_t)((int32_t)x >> s);
}

static inline uint32_t shift_ror(Ctx *ctx, uint32_t x, uint32_t amount, int set_c)
{
    uint32_t s = amount & 0xFF;
    if (s == 0) return x;
    uint32_t r = s & 31;                    /* rotating by 32 is a full turn... */
    uint32_t v = r ? (x >> r) | (x << (32 - r)) : x;
    if (set_c) ctx->c = v >> 31;            /* ...but still sets C from bit 31 */
    return v;
}

#include "vfp.h"

/* clz: count leading zero bits; clz(0) is 32 (C's __builtin_clz(0) is undefined). */
static inline uint32_t arm_clz(uint32_t x) { return x ? (uint32_t)__builtin_clz(x) : 32; }

/* ---- Calls and returns ---- */

/* Every recompiled guest function has this signature. */
typedef void (*GuestFn)(Ctx *ctx);

/* A guest function returning somewhere other than the lr it was called
   with breaks the "host call stack mirrors guest call stack" assumption. */
void recomp_bad_return(Ctx *ctx, uint32_t got, uint32_t want) __attribute__((noreturn));

/* A computed jump (switch table) to an index/address outside the targets
   we recompiled for it. */
void recomp_bad_jump(Ctx *ctx, uint32_t from, uint32_t value) __attribute__((noreturn));

/* Used inside generated functions, which keep their entry lr in `ret`. */
#define RETURN_TO(addr) do {                                    \
        uint32_t to_ = (addr);                                  \
        if (to_ != ret) recomp_bad_return(ctx, to_, ret);       \
        return;                                                 \
    } while (0)

/* ---- Symbol table emitted alongside the generated functions ---- */

typedef struct {
    const char *name;      // guest symbol name
    uint32_t    addr;      // guest address
    GuestFn     fn;        // recompiled host function
} RecompSym;

extern const RecompSym recomp_syms[];
extern const size_t    recomp_nsyms;

/* Find a recompiled function by guest symbol name; NULL if absent. */
GuestFn recomp_lookup(const char *name);

/* Find a recompiled function by its guest entry address, with bit 0 set
   for Thumb functions (as in a function pointer); NULL if there is none. */
GuestFn recomp_find(uint32_t addr);

/* ---- The OS boundary ----
   `svc #n` (a system call into the 3DS kernel) calls recomp_svc, which the
   OS layer sets; arguments and results travel in the registers as on
   hardware. Until a call is implemented it stops with its number. */
typedef void (*SvcHandler)(Ctx *ctx, uint32_t number);
extern SvcHandler recomp_svc_handler;
static inline void recomp_svc(Ctx *ctx, uint32_t number) { recomp_svc_handler(ctx, number); }

/* `udf`: a deliberate trap (e.g. __builtin_trap()); the guest wanted to crash. */
void recomp_udf(Ctx *ctx, uint32_t addr) __attribute__((noreturn));

/* ldrex/strex: exclusive (atomic) access. With one guest thread running
   at a time, the monitor reduces to "was this address ldrex'd, with no
   clrex/strex since?". strex returns 0 on success, 1 on failure. */
static inline uint32_t ldrex32(Ctx *ctx, uint32_t a) { ctx->excl_addr = a; ctx->excl_valid = 1; return read32(ctx, a); }
static inline uint32_t ldrex16(Ctx *ctx, uint32_t a) { ctx->excl_addr = a; ctx->excl_valid = 1; return read16(ctx, a); }
static inline uint32_t ldrex8(Ctx *ctx, uint32_t a)  { ctx->excl_addr = a; ctx->excl_valid = 1; return read8(ctx, a); }

#define STREX_BODY(write)                                       \
    if (!ctx->excl_valid || ctx->excl_addr != a) {              \
        ctx->excl_valid = 0;                                    \
        return 1;                                               \
    }                                                           \
    ctx->excl_valid = 0;                                        \
    write(ctx, a, v);                                           \
    return 0;
static inline uint32_t strex32(Ctx *ctx, uint32_t a, uint32_t v) { STREX_BODY(write32) }
static inline uint32_t strex16(Ctx *ctx, uint32_t a, uint32_t v) { STREX_BODY(write16) }
static inline uint32_t strex8(Ctx *ctx, uint32_t a, uint32_t v)  { STREX_BODY(write8) }
#undef STREX_BODY

/* uadd8: four independent byte additions; GE[i] = carry out of byte i.
   sel: pick each byte from the first operand where GE[i] is set. */
static inline uint32_t arm_uadd8(Ctx *ctx, uint32_t a, uint32_t b)
{
    uint32_t r = 0, ge = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t sum = ((a >> (8 * i)) & 0xFF) + ((b >> (8 * i)) & 0xFF);
        r |= (sum & 0xFF) << (8 * i);
        ge |= (sum >> 8) << i;
    }
    ctx->ge = ge;
    return r;
}

static inline uint32_t arm_sel(Ctx *ctx, uint32_t a, uint32_t b)
{
    uint32_t r = 0;
    for (int i = 0; i < 4; i++)
        r |= (((ctx->ge >> i) & 1) ? a : b) & (0xFFu << (8 * i));
    return r;
}

/* An indirect call/jump to an address no recompiled function starts at. */
void recomp_bad_call(Ctx *ctx, uint32_t addr) __attribute__((noreturn));

/* Indirect calls (function pointers, vtables): the target is only known
   at runtime, so it's looked up in the table of recompiled functions. */
static inline void recomp_call(Ctx *ctx, uint32_t addr)
{
    GuestFn fn = recomp_find(addr);
    if (!fn)
        recomp_bad_call(ctx, addr);
    fn(ctx);
}

/* Copy the program's segments (code, .rodata, .data; zeroed .bss) into
   guest memory at their link addresses. Emitted with the generated code. */
void recomp_load_image(uint8_t *mem);

/* Where those segments live; the list ends with { 0, 0 }. */
typedef struct { uint32_t addr, size; } RecompSegment;
extern const RecompSegment recomp_segments[];

/* Guest address of a data symbol (global variable); 0 if absent. */
uint32_t recomp_data_lookup(const char *name);

/* ---- Guest memory (runtime.c) ---- */

/* Reserve the full 4 GiB guest address space; pages are only backed by
   real memory once touched. */
uint8_t *guest_mem_alloc(void);
void     guest_mem_free(uint8_t *mem);

#endif
