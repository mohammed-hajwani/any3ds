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

void recomp_bad_return(Ctx *ctx, uint32_t got, uint32_t want)
{
    fprintf(stderr,
            "recomp: guest function returned to 0x%08" PRIx32
            " but was called with lr = 0x%08" PRIx32 " (sp = 0x%08" PRIx32 ")\n",
            got, want, ctx->r[13]);
    abort();
}
