/* M6b, first step: actually start the recompiled libctru hello world.
 *
 * Loads the program image, gives it a stack and a thread-local storage
 * block where the 3DS kernel would, and calls its entry point (_start).
 * Every svc it makes is logged; the few that need no real OS are
 * answered, and the first one that needs real services (IPC, memory
 * management, threads) stops the run. That list is the OS layer's to-do.
 */
#define _POSIX_C_SOURCE 199309L     /* clock_gettime */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "context.h"

#define STACK_TOP   0x10000000u     /* top of the main thread's stack on a 3DS */
#define TLS_BASE    0x1FF82000u     /* where Horizon puts the first thread's TLS */
#define EXIT_ADDR   0x00ABC000u     /* sentinel return address */

static const char *svc_name(uint32_t n)
{
    switch (n) {
    case 0x01: return "ControlMemory";      case 0x02: return "QueryMemory";
    case 0x03: return "ExitProcess";        case 0x08: return "CreateThread";
    case 0x09: return "ExitThread";         case 0x0A: return "SleepThread";
    case 0x13: return "CreateMutex";        case 0x14: return "ReleaseMutex";
    case 0x17: return "CreateEvent";        case 0x18: return "SignalEvent";
    case 0x19: return "ClearEvent";         case 0x1F: return "MapMemoryBlock";
    case 0x20: return "UnmapMemoryBlock";   case 0x21: return "CreateAddressArbiter";
    case 0x22: return "ArbitrateAddress";   case 0x23: return "CloseHandle";
    case 0x24: return "WaitSynchronization1"; case 0x25: return "WaitSynchronizationN";
    case 0x27: return "DuplicateHandle";    case 0x28: return "GetSystemTick";
    case 0x2D: return "ConnectToPort";      case 0x32: return "SendSyncRequest";
    case 0x38: return "GetResourceLimit";   case 0x39: return "GetResourceLimitLimitValues";
    case 0x3A: return "GetResourceLimitCurrentValues"; case 0x3C: return "Break";
    default:   return "?";
    }
}

static int svc_count;

static void os_svc(Ctx *ctx, uint32_t n)
{
    printf("svc 0x%02" PRIx32 " %-30s r0-r3 = %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 "  lr = %08" PRIx32 "\n",
           n, svc_name(n), ctx->r[0], ctx->r[1], ctx->r[2], ctx->r[3], ctx->r[14]);
    if (++svc_count > 200) {
        printf("(stopping after 200 svcs)\n");
        exit(2);
    }
    static uint32_t next_handle = 0x100;
    static uint32_t linear_next = 0x14000000u;     /* LINEAR (FCRAM) heap, as on a 3DS */
    switch (n) {
    case 0x21:      /* CreateAddressArbiter(&handle) */
    case 0x13:      /* CreateMutex(&handle, locked) */
    case 0x17:      /* CreateEvent(&handle, reset type) */
        ctx->r[0] = 0;                  /* Result: success */
        ctx->r[1] = next_handle++;
        return;
    case 0x22:      /* ArbitrateAddress: one guest thread, so nothing ever waits */
    case 0x14:      /* ReleaseMutex */
    case 0x18:      /* SignalEvent */
    case 0x19:      /* ClearEvent */
    case 0x23:      /* CloseHandle */
        ctx->r[0] = 0;
        return;
    case 0x01: {    /* ControlMemory(op, addr0, addr1, size, perm) -> r1 = address */
        uint32_t op = ctx->r[0] & 0xFF, linear = ctx->r[0] & 0x10000, size = ctx->r[3];
        if (op == 3) {                  /* MEMOP_ALLOC: our 4 GiB space is all mapped already */
            uint32_t addr = linear ? linear_next : ctx->r[1];
            if (linear)
                linear_next += (size + 0xFFF) & ~0xFFFu;
            printf("    -> allocated %u KiB at %08" PRIx32 "%s\n", size / 1024, addr, linear ? " (linear)" : "");
            ctx->r[0] = 0;
            ctx->r[1] = addr;
            return;
        }
        printf("=> ControlMemory op %" PRIu32 " not handled yet\n", op);
        exit(3);
    }
    case 0x38:      /* GetResourceLimit(&handle, process) */
        ctx->r[0] = 0;
        ctx->r[1] = next_handle++;
        return;
    case 0x39:      /* GetResourceLimitLimitValues(s64 *values, handle, u32 *names, count) */
    case 0x3A: {    /* GetResourceLimitCurrentValues(...) */
        for (uint32_t i = 0; i < ctx->r[3]; i++) {
            uint32_t name;
            memcpy(&name, ctx->mem + ctx->r[2] + 4 * i, 4);
            /* name 1 = COMMIT (bytes of memory): 64 MiB budget, 2 MiB in use */
            int64_t value = name == 1 ? (n == 0x39 ? 0x04000000 : 0x00200000) : (n == 0x39 ? 32 : 1);
            memcpy(ctx->mem + ctx->r[0] + 8 * i, &value, 8);
        }
        ctx->r[0] = 0;
        return;
    }
    case 0x28: {    /* GetSystemTick: the 268 MHz ARM11 tick counter */
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t t = (uint64_t)ts.tv_sec * 268111856u + (uint64_t)ts.tv_nsec * 268111856u / 1000000000u;
        ctx->r[0] = (uint32_t)t;
        ctx->r[1] = (uint32_t)(t >> 32);
        return;
    }
    case 0x03:      /* ExitProcess */
        printf("guest exited via svcExitProcess after %d svcs\n", svc_count);
        exit(0);
    default:
        printf("=> needs the OS layer: stopping here (svc #%d)\n", svc_count);
        exit(3);
    }
}

int main(void)
{
    uint8_t *mem = guest_mem_alloc();
    recomp_load_image(mem);

    GuestFn start = recomp_find(recomp_entry);
    if (!start) {
        printf("no recompiled function at the entry point %08x\n", recomp_entry);
        return 1;
    }
    recomp_svc_handler = os_svc;

    Ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.mem = mem;
    ctx.r[13] = STACK_TOP;
    ctx.r[14] = EXIT_ADDR;
    ctx.tls = TLS_BASE;
    printf("starting at the entry point %08x with sp = %08x, tls = %08x\n", recomp_entry, STACK_TOP, TLS_BASE);
    start(&ctx);
    printf("_start returned (r0 = %08" PRIx32 ") after %d svcs\n", ctx.r[0], svc_count);
    return 0;
}
