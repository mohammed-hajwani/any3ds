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
} Ctx;

static inline uint32_t read32(Ctx *c, uint32_t a)  { uint32_t v; memcpy(&v, c->mem + a, 4); return v; }
static inline void     write32(Ctx *c, uint32_t a, uint32_t v) { memcpy(c->mem + a, &v, 4); }

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

/* ---- Calls and returns ---- */

/* Every recompiled guest function has this signature. */
typedef void (*GuestFn)(Ctx *ctx);

/* A guest function returning somewhere other than the lr it was called
   with breaks the "host call stack mirrors guest call stack" assumption. */
void recomp_bad_return(Ctx *ctx, uint32_t got, uint32_t want) __attribute__((noreturn));

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

/* ---- Guest memory (runtime.c) ---- */

/* Reserve the full 4 GiB guest address space; pages are only backed by
   real memory once touched. */
uint8_t *guest_mem_alloc(void);
void     guest_mem_free(uint8_t *mem);

#endif
