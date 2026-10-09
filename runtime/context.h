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

/* Every recompiled guest function has this signature. */
typedef void (*GuestFn)(Ctx *ctx);

/* Symbol table emitted alongside the generated functions. */
typedef struct {
    const char *name;      // guest symbol name
    uint32_t    addr;      // guest address
    GuestFn     fn;        // recompiled host function
} RecompSym;

extern const RecompSym recomp_syms[];
extern const size_t    recomp_nsyms;

/* Find a recompiled function by guest symbol name; NULL if absent. */
GuestFn recomp_lookup(const char *name);

#endif
