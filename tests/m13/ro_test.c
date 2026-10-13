/* M13 test: a libctru program that loads two CRO modules built from our own
 * code (tests/m13/mkcro.py) through ldr:ro, the way a game does, and calls
 * into them. Module A is loaded first and imports from B, which is loaded
 * second (so the loader must patch A when B arrives); then A is unloaded
 * and loaded again at another address. The output must match the same
 * modules compiled natively (native_main.c).
 *
 * The modules' code was recompiled ahead of time relative to their load
 * address (recomp --cro); our ldr:ro (hle/ro.c) maps and fixes them. */
#include <3ds.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include "sequence.h"

static Handle ro;

typedef struct {
    u8 *file;           /* where the .cro was read (the heap) */
    u32 mapped;         /* where it is loaded */
    u32 size;
    u8 *data, *bss;
} Module;

static u32 rd(u32 addr) { u32 v; memcpy(&v, (void *)addr, 4); return v; }

static void read_file(const char *path, Module *m)
{
    FILE *f = fopen(path, "rb");
    fseek(f, 0, SEEK_END);
    m->size = (u32)ftell(f);
    fseek(f, 0, SEEK_SET);
    m->file = memalign(0x1000, m->size);
    fread(m->file, 1, m->size, f);
    fclose(f);
}

static Result load(Module *m, u32 mapped)
{
    u32 data_size = rd((u32)m->file + 0xBC), bss_size = rd((u32)m->file + 0x94);
    m->mapped = mapped;
    m->data = memalign(0x1000, data_size + 1);
    m->bss = memalign(0x1000, bss_size + 1);
    u32 *cmd = getThreadCommandBuffer();
    cmd[0] = IPC_MakeHeader(0x9, 11, 2);        /* LoadCRO_New */
    cmd[1] = (u32)m->file;
    cmd[2] = mapped;
    cmd[3] = m->size;
    cmd[4] = (u32)m->data;
    cmd[5] = 0;
    cmd[6] = data_size;
    cmd[7] = (u32)m->bss;
    cmd[8] = bss_size;
    cmd[9] = 1;                                 /* auto-link */
    cmd[10] = 1;                                /* fix level */
    cmd[11] = 0;
    cmd[12] = IPC_Desc_SharedHandles(1);
    cmd[13] = CUR_PROCESS_HANDLE;
    Result rc = svcSendSyncRequest(ro);
    return R_FAILED(rc) ? rc : (Result)cmd[1];
}

static void unload(Module *m)
{
    u32 *cmd = getThreadCommandBuffer();
    cmd[0] = IPC_MakeHeader(0x5, 3, 2);
    cmd[1] = m->mapped;
    cmd[2] = 0;
    cmd[3] = (u32)m->file;
    cmd[4] = IPC_Desc_SharedHandles(1);
    cmd[5] = CUR_PROCESS_HANDLE;
    svcSendSyncRequest(ro);
}

/* An export's address, from the loaded module's tables (fixed by the
   loader: table offsets and segment places are addresses now). */
static void *export_addr(Module *m, const char *name)
{
    u32 base = m->mapped, table = rd(base + 0xD0), num = rd(base + 0xD4), segs = rd(base + 0xC8);
    for (u32 i = 0; i < num; i++) {
        if (strcmp((const char *)rd(table + 8 * i), name) == 0) {
            u32 segoff = rd(table + 8 * i + 4);
            return (void *)(rd(segs + 12 * (segoff & 0xF)) + (segoff >> 4));
        }
    }
    fprintf(stderr, "export %s not found\n", name);
    return NULL;
}

static Module a, b;
static u32 next_mapped = 0x00800000;

static void find_exports(Exports *e)
{
    e->a_compute = export_addr(&a, "a_compute");
    e->b_add = export_addr(&b, "b_add");
    e->b_count = export_addr(&b, "b_count");
    e->b_counter = export_addr(&b, "b_counter");
    e->a_state = export_addr(&a, "a_state");
}

static void reload_a(Exports *e)
{
    unload(&a);
    next_mapped += 0x10000;                     /* somewhere else this time */
    Result rc = load(&a, next_mapped);
    if (R_FAILED(rc))
        fprintf(stderr, "reloading module A failed: %08lx\n", rc);
    find_exports(e);
}

static void say(const char *line) { fprintf(stderr, "%s\n", line); }

int main(void)
{
    consoleDebugInit(debugDevice_SVC);
    if (R_FAILED(srvGetServiceHandle(&ro, "ldr:ro"))) {
        fprintf(stderr, "no ldr:ro\n");
        return 1;
    }
    read_file("sdmc:/mod_a.cro", &a);
    read_file("sdmc:/mod_b.cro", &b);
    Result ra = load(&a, next_mapped);
    next_mapped += 0x10000;
    Result rb = load(&b, next_mapped);
    if (R_FAILED(ra) || R_FAILED(rb)) {
        fprintf(stderr, "LoadCRO failed: %08lx %08lx\n", ra, rb);
        return 1;
    }
    Exports e;
    find_exports(&e);
    run_sequence(&e, reload_a, say);
    return 0;
}
