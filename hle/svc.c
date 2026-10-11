/* svc number -> kernel call. Arguments and results travel in registers,
 * exactly as libctru's svc wrappers put them (see their disassembly:
 * e.g. svcCreateThread moves the priority into r0 and the core into r4). */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel.h"
#include "service.h"

static const char *svc_name(uint32_t n)
{
    switch (n) {
    case 0x01: return "ControlMemory";        case 0x02: return "QueryMemory";
    case 0x03: return "ExitProcess";          case 0x08: return "CreateThread";
    case 0x09: return "ExitThread";           case 0x0A: return "SleepThread";
    case 0x0B: return "GetThreadPriority";    case 0x0C: return "SetThreadPriority";
    case 0x13: return "CreateMutex";          case 0x14: return "ReleaseMutex";
    case 0x15: return "CreateSemaphore";      case 0x16: return "ReleaseSemaphore";
    case 0x17: return "CreateEvent";          case 0x18: return "SignalEvent";
    case 0x19: return "ClearEvent";           case 0x1E: return "CreateMemoryBlock";
    case 0x1F: return "MapMemoryBlock";
    case 0x20: return "UnmapMemoryBlock";     case 0x21: return "CreateAddressArbiter";
    case 0x22: return "ArbitrateAddress";     case 0x23: return "CloseHandle";
    case 0x24: return "WaitSynchronization1"; case 0x25: return "WaitSynchronizationN";
    case 0x27: return "DuplicateHandle";      case 0x28: return "GetSystemTick";
    case 0x2D: return "ConnectToPort";        case 0x32: return "SendSyncRequest";
    case 0x35: return "GetProcessId";         case 0x37: return "GetThreadId";
    case 0x38: return "GetResourceLimit";     case 0x39: return "GetResourceLimitLimitValues";
    case 0x3A: return "GetResourceLimitCurrentValues";
    case 0x3C: return "Break";                case 0x3D: return "OutputDebugString";
    default:   return "?";
    }
}

static int64_t s64_of(uint32_t lo, uint32_t hi)
{
    return (int64_t)((uint64_t)hi << 32 | lo);
}

/* The handle's object if it has the expected type, else NULL (logged). */
static KObj *get(Ctx *ctx, uint32_t h, KType type)
{
    KObj *o = handle_get(h);
    if (o && o->type == type)
        return o;
    fprintf(stderr, "hle: bad handle %08" PRIx32 " (want type %d, have %s) at lr = %08" PRIx32 "\n",
            h, type, o ? o->name : "nothing", ctx->r[14]);
    return NULL;
}

#define GET_OR_FAIL(var, h, type)               \
    KObj *var = get(ctx, (h), (type));          \
    if (!var) {                                 \
        ctx->r[0] = RES_INVALID_HANDLE;         \
        break;                                  \
    }

/* ---- Memory ----
   The whole 4 GiB guest space is backed already, so allocating is only
   handing out addresses. But the app can ask what is where
   (svcQueryMemory: libctru uses it to find free room for shared memory),
   so we keep a map of what is in use. */
enum { MEMSTATE_FREE = 0, MEMSTATE_CODE = 4, MEMSTATE_PRIVATE = 5, MEMSTATE_SHARED = 6, MEMSTATE_CONTINUOUS = 7 };
enum { PERM_R = 1, PERM_W = 2, PERM_X = 4 };

typedef struct { uint32_t base, size, perm, state; } Region;

#define MAX_REGIONS 64
static Region regions[MAX_REGIONS];
static int nregions;

static uint32_t page_round(uint32_t size) { return (size + 0xFFF) & ~0xFFFu; }

static void region_add(uint32_t base, uint32_t size, uint32_t perm, uint32_t state)
{
    if (nregions == MAX_REGIONS) {
        fprintf(stderr, "hle: too many memory regions\n");
        exit(3);
    }
    int i = nregions++;             /* keep them sorted by address */
    while (i > 0 && regions[i - 1].base > base) {
        regions[i] = regions[i - 1];
        i--;
    }
    regions[i] = (Region){ base & ~0xFFFu, page_round(size), perm, state };
}

static void region_remove(uint32_t base)
{
    for (int i = 0; i < nregions; i++)
        if (regions[i].base == (base & ~0xFFFu)) {
            memmove(&regions[i], &regions[i + 1], (size_t)(nregions - i - 1) * sizeof regions[0]);
            nregions--;
            return;
        }
}

/* The program image and the main thread's stack, mapped at startup. */
static void regions_init(void)
{
    static int done;
    if (done)
        return;
    done = 1;
    for (const RecompSegment *s = recomp_segments; s->size; s++)
        region_add(s->addr, s->size, PERM_R | PERM_W | PERM_X, MEMSTATE_CODE);
    region_add(0x10000000u - 0x100000u, 0x100000u, PERM_R | PERM_W, MEMSTATE_PRIVATE);
}

/* QueryMemory(addr) -> base, size, permissions, state (and page flags). */
static void query_memory(Ctx *ctx)
{
    regions_init();
    uint32_t addr = ctx->r[2], free_base = 0, free_end = 0;
    for (int i = 0; i < nregions; i++) {
        const Region *r = &regions[i];
        if (addr >= r->base && addr - r->base < r->size) {
            ctx->r[1] = r->base;
            ctx->r[2] = r->size;
            ctx->r[3] = r->perm;
            ctx->r[4] = r->state;
            goto done;
        }
        if (r->base <= addr)
            free_base = r->base + r->size;
        else if (!free_end)
            free_end = r->base;
    }
    ctx->r[1] = free_base;          /* a free block: from the region before to the one after */
    ctx->r[2] = (free_end ? free_end : 0xFFFFF000u) - free_base;
    ctx->r[3] = 0;
    ctx->r[4] = MEMSTATE_FREE;
done:
    ctx->r[0] = RES_OK;
    ctx->r[5] = 0;
}

static uint32_t linear_next = 0x14000000u;      /* LINEAR (FCRAM) heap, as on a 3DS */

static void control_memory(Ctx *ctx)
{
    uint32_t op = ctx->r[0] & 0xFF, linear = ctx->r[0] & 0x10000, size = ctx->r[3];
    regions_init();
    if (op == 3) {                  /* MEMOP_ALLOC */
        uint32_t addr = linear ? linear_next : ctx->r[1];
        if (linear)
            linear_next += page_round(size);
        region_add(addr, size, PERM_R | PERM_W, linear ? MEMSTATE_CONTINUOUS : MEMSTATE_PRIVATE);
        if (hle_trace)
            fprintf(stderr, "    -> allocated %" PRIu32 " KiB at %08" PRIx32 "%s\n",
                    size / 1024, addr, linear ? " (linear)" : "");
        ctx->r[0] = RES_OK;
        ctx->r[1] = addr;
        return;
    }
    if (op == 1) {                  /* MEMOP_FREE */
        region_remove(ctx->r[1]);
        ctx->r[0] = RES_OK;
        return;
    }
    fprintf(stderr, "hle: ControlMemory operation %" PRIu32 " is not implemented\n", op);
    exit(3);
}

static void resource_limit_values(Ctx *ctx, uint32_t n)
{
    uint8_t *mem = kernel_mem();
    for (uint32_t i = 0; i < ctx->r[3]; i++) {
        uint32_t name;
        memcpy(&name, mem + ctx->r[2] + 4 * i, 4);
        /* name 1 = COMMIT (bytes of memory): a 64 MiB budget, 2 MiB in use */
        int64_t value = name == 1 ? (n == 0x39 ? 0x04000000 : 0x00200000) : (n == 0x39 ? 32 : 1);
        memcpy(mem + ctx->r[0] + 8 * i, &value, 8);
    }
    ctx->r[0] = RES_OK;
}

void hle_svc(Ctx *ctx, uint32_t n);

void hle_svc(Ctx *ctx, uint32_t n)
{
    k_svc_enter();
    if (hle_trace)
        fprintf(stderr, "[%08" PRIx32 "] svc 0x%02" PRIx32 " %-24s r0-r3 = %08" PRIx32 " %08" PRIx32 " %08" PRIx32
                " %08" PRIx32 "  lr = %08" PRIx32 "\n", k_thread_id(NULL), n, svc_name(n),
                ctx->r[0], ctx->r[1], ctx->r[2], ctx->r[3], ctx->r[14]);

    switch (n) {
    case 0x01:
        control_memory(ctx);
        break;
    case 0x02:
        query_memory(ctx);
        break;
    case 0x03:
        k_exit_process();
    case 0x08: {    /* CreateThread: r0 priority, r1 entry, r2 arg, r3 stack top, r4 core */
        uint32_t h = 0;
        ctx->r[0] = k_create_thread(ctx->r[1], ctx->r[2], ctx->r[3], (int)ctx->r[0], &h);
        ctx->r[1] = h;
        break;
    }
    case 0x09:
        k_exit_thread();
    case 0x0A:      /* SleepThread(s64 ns) */
        k_sleep(s64_of(ctx->r[0], ctx->r[1]));
        break;
    case 0x0B: {    /* GetThreadPriority(&prio, handle) */
        GET_OR_FAIL(t, ctx->r[1], KO_THREAD);
        ctx->r[0] = RES_OK;
        ctx->r[1] = (uint32_t)k_thread_priority(t);
        break;
    }
    case 0x0C: {    /* SetThreadPriority(handle, prio) */
        GET_OR_FAIL(t, ctx->r[0], KO_THREAD);
        k_set_thread_priority(t, (int)ctx->r[1]);
        ctx->r[0] = RES_OK;
        break;
    }
    case 0x13: {    /* CreateMutex(&handle, initially locked) */
        KObj *m = kobj_new(KO_MUTEX, "mutex");
        if (ctx->r[1]) {
            m->mtx.owner = k_current_thread_obj()->thread;
            m->mtx.count = 1;
        }
        ctx->r[0] = RES_OK;
        ctx->r[1] = handle_new(m);
        break;
    }
    case 0x14: {
        GET_OR_FAIL(m, ctx->r[0], KO_MUTEX);
        ctx->r[0] = k_release_mutex(m);
        break;
    }
    case 0x15: {    /* CreateSemaphore(&handle, initial, max) */
        KObj *s = kobj_new(KO_SEMAPHORE, "semaphore");
        s->sem.count = (int32_t)ctx->r[1];
        s->sem.max = (int32_t)ctx->r[2];
        ctx->r[0] = RES_OK;
        ctx->r[1] = handle_new(s);
        break;
    }
    case 0x16: {    /* ReleaseSemaphore(&previous, handle, count) */
        GET_OR_FAIL(s, ctx->r[1], KO_SEMAPHORE);
        int32_t prev = 0;
        ctx->r[0] = k_release_semaphore(s, (int32_t)ctx->r[2], &prev);
        ctx->r[1] = (uint32_t)prev;
        break;
    }
    case 0x17: {    /* CreateEvent(&handle, reset type) */
        ctx->r[1] = handle_new(kevent_new((int)ctx->r[1], "event"));
        ctx->r[0] = RES_OK;
        break;
    }
    case 0x18: {
        GET_OR_FAIL(e, ctx->r[0], KO_EVENT);
        kevent_signal(e);
        ctx->r[0] = RES_OK;
        break;
    }
    case 0x19: {
        GET_OR_FAIL(e, ctx->r[0], KO_EVENT);
        e->ev.signaled = 0;
        ctx->r[0] = RES_OK;
        break;
    }
    case 0x1E: {    /* CreateMemoryBlock(&handle, addr, size, my perm, other perm): r0 other perm, r1 addr,
                       r2 size, r3 my perm. Memory the app shares out of its own space, e.g. for sockets. */
        if (!ctx->r[1]) {
            fprintf(stderr, "hle: CreateMemoryBlock with a kernel-chosen address is not implemented\n");
            exit(3);
        }
        KObj *m = kobj_new(KO_SHMEM, "app memory block");
        m->shm.addr = ctx->r[1];
        m->shm.size = ctx->r[2];
        ctx->r[0] = RES_OK;
        ctx->r[1] = handle_new(m);
        break;
    }
    case 0x1F: {    /* MapMemoryBlock(handle, addr, my perm, other perm) */
        GET_OR_FAIL(m, ctx->r[0], KO_SHMEM);
        regions_init();
        m->shm.addr = ctx->r[1];
        memset(kernel_mem() + m->shm.addr, 0, m->shm.size);
        region_add(m->shm.addr, m->shm.size, ctx->r[2], MEMSTATE_SHARED);
        if (hle_trace)
            fprintf(stderr, "    -> %s mapped at %08" PRIx32 "\n", m->name, m->shm.addr);
        ctx->r[0] = RES_OK;
        break;
    }
    case 0x20: {    /* UnmapMemoryBlock(handle, addr) */
        GET_OR_FAIL(m, ctx->r[0], KO_SHMEM);
        region_remove(m->shm.addr);
        m->shm.addr = 0;
        ctx->r[0] = RES_OK;
        break;
    }
    case 0x21:      /* CreateAddressArbiter(&handle) */
        ctx->r[1] = handle_new(kobj_new(KO_ARBITER, "arbiter"));
        ctx->r[0] = RES_OK;
        break;
    case 0x22: {    /* ArbitrateAddress(arbiter, addr, type, value, s64 timeout in r4/r5) */
        GET_OR_FAIL(a, ctx->r[0], KO_ARBITER);
        k_arbitrate(a, ctx->r[1], ctx->r[2], (int32_t)ctx->r[3], s64_of(ctx->r[4], ctx->r[5]));
        break;
    }
    case 0x23:
        k_close_handle(ctx->r[0]);
        ctx->r[0] = RES_OK;
        break;
    case 0x24: {    /* WaitSynchronization1(handle, s64 timeout in r2/r3) */
        KObj *o = handle_get(ctx->r[0]);
        if (!o) {
            fprintf(stderr, "hle: wait on bad handle %08" PRIx32 " at lr = %08" PRIx32 "\n", ctx->r[0], ctx->r[14]);
            ctx->r[0] = RES_INVALID_HANDLE;
            break;
        }
        k_wait(&o, 1, 0, s64_of(ctx->r[2], ctx->r[3]));
        break;
    }
    case 0x25: {    /* WaitSynchronizationN: r1 handles, r2 count, r3 wait all, timeout r0 + r4 */
        KObj *objs[64];
        int count = (int)ctx->r[2], bad = 0;
        if (count < 0 || count > 64) {
            ctx->r[0] = RES_OUT_OF_RANGE;
            break;
        }
        for (int i = 0; i < count; i++) {
            uint32_t h;
            memcpy(&h, kernel_mem() + ctx->r[1] + 4 * (uint32_t)i, 4);
            if (!(objs[i] = handle_get(h)))
                bad = 1;
        }
        if (bad) {
            ctx->r[0] = RES_INVALID_HANDLE;
            break;
        }
        k_wait(objs, count, ctx->r[3] != 0, s64_of(ctx->r[0], ctx->r[4]));
        break;
    }
    case 0x27: {    /* DuplicateHandle(&new, handle) */
        KObj *o = handle_get(ctx->r[1]);
        ctx->r[0] = o ? RES_OK : RES_INVALID_HANDLE;
        ctx->r[1] = o ? handle_new(o) : 0;
        break;
    }
    case 0x28: {    /* GetSystemTick */
        uint64_t t = kernel_now();
        ctx->r[0] = (uint32_t)t;
        ctx->r[1] = (uint32_t)(t >> 32);
        break;
    }
    case 0x2D: {    /* ConnectToPort(&handle, name) */
        char name[12] = { 0 };
        memcpy(name, kernel_mem() + ctx->r[1], 11);
        KObj *session = port_connect(name);
        ctx->r[0] = session ? RES_OK : RES_NOT_FOUND;
        ctx->r[1] = session ? handle_new(session) : 0;
        break;
    }
    case 0x32: {    /* SendSyncRequest(session): an IPC request through TLS + 0x80 */
        GET_OR_FAIL(s, ctx->r[0], KO_SESSION);
        ipc_request(s);
        ctx->r[0] = RES_OK;
        break;
    }
    case 0x35:      /* GetProcessId(&id, handle) */
        ctx->r[0] = RES_OK;
        ctx->r[1] = 0x28;
        break;
    case 0x37: {    /* GetThreadId(&id, handle) */
        GET_OR_FAIL(t, ctx->r[1], KO_THREAD);
        ctx->r[0] = RES_OK;
        ctx->r[1] = k_thread_id(t);
        break;
    }
    case 0x38:      /* GetResourceLimit(&handle, process) */
        ctx->r[1] = handle_new(kobj_new(KO_RESLIMIT, "resource limit"));
        ctx->r[0] = RES_OK;
        break;
    case 0x39:
    case 0x3A:
        resource_limit_values(ctx, n);
        break;
    case 0x3C:      /* Break(reason) */
        fprintf(stderr, "hle: guest called svcBreak(%" PRIu32 ") at lr = %08" PRIx32 "\n", ctx->r[0], ctx->r[14]);
        exit(5);
    case 0x3D:      /* OutputDebugString(str, length) */
        fwrite(kernel_mem() + ctx->r[0], 1, ctx->r[1], stdout);
        fflush(stdout);
        ctx->r[0] = RES_OK;
        break;
    default:
        fprintf(stderr, "hle: svc 0x%02" PRIx32 " (%s) is not implemented (r0-r3 = %08" PRIx32 " %08" PRIx32
                " %08" PRIx32 " %08" PRIx32 ", lr = %08" PRIx32 ")\n",
                n, svc_name(n), ctx->r[0], ctx->r[1], ctx->r[2], ctx->r[3], ctx->r[14]);
        exit(3);
    }
    k_svc_leave();
}
