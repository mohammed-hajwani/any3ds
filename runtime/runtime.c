/* Host-side runtime support for recompiled code. */
#define _DEFAULT_SOURCE         /* MAP_ANONYMOUS, MAP_NORESERVE */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>

#include "context.h"

#define GUEST_SPACE (UINT64_C(1) << 32)

uint8_t *guest_mem_alloc(void)
{
    void *p = mmap(NULL, GUEST_SPACE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        perror("guest_mem_alloc: mmap 4 GiB");
        exit(1);
    }
    return p;
}

void guest_mem_free(uint8_t *mem)
{
    munmap(mem, GUEST_SPACE);
}

void recomp_bad_jump(Ctx *ctx, uint32_t from, uint32_t value)
{
    fprintf(stderr,
            "recomp: computed jump at 0x%08" PRIx32 " with value 0x%08" PRIx32
            ", outside the recompiled targets (lr = 0x%08" PRIx32 ")\n",
            from, value, ctx->r[14]);
    abort();
}

void recomp_untranslated(Ctx *ctx, uint32_t addr, const char *why)
{
    fprintf(stderr, "recomp: function 0x%08" PRIx32 " was not translated (recompiled with --keep-going): %s"
            " (lr = 0x%08" PRIx32 ")\n", addr, why, ctx->r[14]);
    abort();
}

static void svc_unimplemented(Ctx *ctx, uint32_t number)
{
    fprintf(stderr, "recomp: svc 0x%02" PRIx32 " is not implemented by the OS layer"
            " (r0-r3 = %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 ", lr = 0x%08" PRIx32 ")\n",
            number, ctx->r[0], ctx->r[1], ctx->r[2], ctx->r[3], ctx->r[14]);
    abort();
}

SvcHandler recomp_svc_handler = svc_unimplemented;

/* ---- CRO modules (see context.h) */

/* No modules unless the program links a table of them (tools/build_dump.sh). */
__attribute__((weak)) const RecompModule *const *recomp_module_table(void)
{
    static const RecompModule *const none[] = { NULL };
    return none;
}

#define MAX_ATTACHED 64
static struct { const RecompModule *m; uint32_t base, size; } attached[MAX_ATTACHED];
static int nattached;

const RecompModule *recomp_module_named(const char *name)
{
    const RecompModule *const *mods = recomp_module_table();
    for (int i = 0; mods[i]; i++)
        if (strcmp(mods[i]->name, name) == 0)
            return mods[i];
    return NULL;
}

void recomp_module_attach(const RecompModule *m, uint32_t base, uint32_t size)
{
    recomp_module_detach(m);
    if (nattached == MAX_ATTACHED) {
        fprintf(stderr, "recomp: more than %d CRO modules loaded at once\n", MAX_ATTACHED);
        abort();
    }
    *m->base = base;
    attached[nattached].m = m;
    attached[nattached].base = base;
    attached[nattached].size = size;
    nattached++;
}

void recomp_module_detach(const RecompModule *m)
{
    for (int i = 0; i < nattached; i++)
        if (attached[i].m == m) {
            attached[i] = attached[--nattached];
            return;
        }
}

/* The main program's functions: generated code defines this (emit_c.py);
   a test that links no recompiled program gets none. */
__attribute__((weak)) GuestFn recomp_find_static(uint32_t addr)
{
    (void)addr;
    return NULL;
}

GuestFn recomp_find(uint32_t addr)
{
    GuestFn fn = recomp_find_static(addr);
    if (fn)
        return fn;
    for (int i = 0; i < nattached; i++) {
        uint32_t off = addr - attached[i].base;
        if (off >= attached[i].size)
            continue;
        const RecompModuleFn *f = attached[i].m->fns;
        size_t lo = 0, hi = attached[i].m->nfns;
        while (lo < hi) {           /* binary search by offset (bit 0 = Thumb) */
            size_t mid = lo + (hi - lo) / 2;
            if (f[mid].offset == off)
                return f[mid].fn;
            if (f[mid].offset < off)
                lo = mid + 1;
            else
                hi = mid;
        }
    }
    return NULL;
}

TickHandler recomp_tick_handler;   /* NULL: no OS layer, time doesn't matter */

void recomp_tick(Ctx *ctx)
{
    ctx->budget = RECOMP_TICK_EDGES;
    if (recomp_tick_handler)
        recomp_tick_handler(ctx);
}

void recomp_bad_fpscr(Ctx *ctx, uint32_t value)
{
    fprintf(stderr, "recomp: guest set FPSCR = 0x%08" PRIx32 ": a rounding mode, short-vector mode or"
            " trap enable that is not modelled (lr = 0x%08" PRIx32 ")\n", value, ctx->r[14]);
    abort();
}

void recomp_no_return(Ctx *ctx, uint32_t addr)
{
    fprintf(stderr, "recomp: the call at 0x%08" PRIx32 " was expected never to return, but it did"
            " (lr = 0x%08" PRIx32 ")\n", addr, ctx->r[14]);
    abort();
}

void recomp_udf(Ctx *ctx, uint32_t addr)
{
    fprintf(stderr, "recomp: guest executed udf (a deliberate trap) at 0x%08" PRIx32 " (lr = 0x%08" PRIx32 ")\n",
            addr, ctx->r[14]);
    abort();
}

void recomp_undefined(Ctx *ctx, uint32_t addr, const char *text)
{
    fprintf(stderr, "recomp: guest executed `%s` at 0x%08" PRIx32 ", an undefined instruction on the 3DS's ARM11"
            " (lr = 0x%08" PRIx32 ")\n", text, addr, ctx->r[14]);
    abort();
}

void recomp_bad_call(Ctx *ctx, uint32_t addr)
{
    fprintf(stderr,
            "recomp: indirect call/jump to 0x%08" PRIx32 ", where no recompiled %s function starts"
            " (lr = 0x%08" PRIx32 ")\n",
            addr, addr & 1 ? "Thumb" : "ARM", ctx->r[14]);
    fprintf(stderr, "    r0-r7  = %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
            " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 "\n",
            ctx->r[0], ctx->r[1], ctx->r[2], ctx->r[3], ctx->r[4], ctx->r[5], ctx->r[6], ctx->r[7]);
    fprintf(stderr, "    r8-r13 = %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 "\n",
            ctx->r[8], ctx->r[9], ctx->r[10], ctx->r[11], ctx->r[12], ctx->r[13]);
    abort();
}

void recomp_bad_return(Ctx *ctx, uint32_t got, uint32_t want)
{
    fprintf(stderr,
            "recomp: guest function returned to 0x%08" PRIx32
            " but was called with lr = 0x%08" PRIx32 " (sp = 0x%08" PRIx32 ")\n",
            got, want, ctx->r[13]);
    abort();
}
