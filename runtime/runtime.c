/* Host-side runtime support for recompiled code. */
#define _DEFAULT_SOURCE         /* MAP_ANONYMOUS, MAP_NORESERVE */

#include <inttypes.h>
#include <stdio.h>
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

static void svc_unimplemented(Ctx *ctx, uint32_t number)
{
    fprintf(stderr, "recomp: svc 0x%02" PRIx32 " is not implemented by the OS layer"
            " (r0-r3 = %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 ", lr = 0x%08" PRIx32 ")\n",
            number, ctx->r[0], ctx->r[1], ctx->r[2], ctx->r[3], ctx->r[14]);
    abort();
}

SvcHandler recomp_svc_handler = svc_unimplemented;

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
