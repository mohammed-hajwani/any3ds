/* ldr:ro, the loader for CRO modules (the 3DS's "DLLs"), from 3dbrew's
 * RO_Services, RO:* and CRO0 pages.
 *
 * The game reads a .cro from its RomFS into its heap and asks us to load
 * it: we map it at the address it chose, give its .data and .bss the
 * buffers it supplied, and fix it up as RO does:
 *   - the header's table offsets become addresses, and so does each
 *     segment's place (text/rodata stay in the mapped file, data/bss move
 *     to the game's buffers);
 *   - internal patches (its own pointers) are applied;
 *   - imports are resolved against the exports of the main program
 *     (static.crs, given to Initialize) and of every loaded module, and
 *     modules loaded earlier that import from this one get patched too.
 * The module's code was recompiled ahead of time relative to its load
 * address (recomp --cro); we attach it at the base, so its functions run
 * and recomp_find finds them. A module we have no recompiled code for is a
 * hard stop that names it. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "service.h"

#define MAX_MODULES 64
#define SEG_TEXT 0
#define SEG_RODATA 1
#define SEG_DATA 2
#define SEG_BSS 3
#define R_ARM_ABS32 2
#define R_ARM_TARGET1 38
#define NO_OFFSET 0xFFFFFFFFu

typedef struct {
    int used;
    int is_static;              /* the CRS: the main program's exports and imports */
    char name[64];
    uint32_t base;              /* where the file is mapped */
    uint32_t size;
    uint32_t seg_addr[16];      /* where each segment is */
    uint32_t seg_size[16];
    int nsegs;
    const RecompModule *code;   /* its recompiled code */
    uint8_t *pristine;          /* the file as the game gave it, to hand back on unload */
} Loaded;

static Loaded mods[MAX_MODULES];

static uint8_t *g(uint32_t addr) { return kernel_mem() + addr; }
static uint32_t rd(uint32_t addr) { uint32_t v; memcpy(&v, g(addr), 4); return v; }
static void wr(uint32_t addr, uint32_t v) { memcpy(g(addr), &v, 4); }
/* Header fields hold offsets from the start of the file (before fixing). */
static uint32_t hdr(const Loaded *m, uint32_t field) { return rd(m->base + field); }

static uint32_t seg_offset_addr(const Loaded *m, uint32_t segoff)
{
    uint32_t seg = segoff & 0xF, off = segoff >> 4;
    if ((int)seg >= m->nsegs) {
        fprintf(stderr, "hle: RO: %s: segment %" PRIu32 " out of range\n", m->name, seg);
        exit(3);
    }
    return m->seg_addr[seg] + off;
}

static void apply(const Loaded *m, uint32_t entry, uint32_t value)
{
    uint32_t at = seg_offset_addr(m, rd(entry)), type = g(entry)[4];
    int32_t addend;
    memcpy(&addend, g(entry + 8), 4);
    switch (type) {
    case 0:
        return;
    case R_ARM_ABS32:
    case R_ARM_TARGET1:
        wr(at, value + (uint32_t)addend);
        return;
    default:
        fprintf(stderr, "hle: RO: %s: relocation type %" PRIu32 " is not implemented yet\n", m->name, type);
        exit(3);
    }
}

/* Walk an import's patch list (entries until the one flagged "last"). */
static void apply_import(const Loaded *m, uint32_t list, uint32_t value)
{
    for (int n = 0; n < 1 << 20; n++, list += 12) {
        apply(m, list, value);
        if (g(list)[5])
            return;
    }
}

static Loaded *find_loaded(const char *name)
{
    for (int i = 0; i < MAX_MODULES; i++)
        if (mods[i].used && strcmp(mods[i].name, name) == 0)
            return &mods[i];
    return NULL;
}

static void cstr(uint32_t addr, char *out, size_t cap)
{
    size_t i = 0;
    for (; i + 1 < cap && g(addr)[i]; i++)
        out[i] = (char)g(addr)[i];
    out[i] = 0;
}

/* The address of an export of `m` by name, or 0. */
static uint32_t export_named(const Loaded *m, const char *name)
{
    uint32_t table = hdr(m, 0xD0), num = hdr(m, 0xD4);
    char buf[256];
    for (uint32_t i = 0; i < num; i++) {
        cstr(rd(table + 8 * i), buf, sizeof buf);
        if (strcmp(buf, name) == 0)
            return seg_offset_addr(m, rd(table + 8 * i + 4));
    }
    return 0;
}

/* Resolve `importer`'s imports that refer to `exporter` (or, with
   exporter == NULL, every import, against everything loaded). */
static void link_imports(const Loaded *importer, const Loaded *only)
{
    /* Named imports: any module may export the name. */
    uint32_t table = hdr(importer, 0x100), num = hdr(importer, 0x104);
    char name[256];
    for (uint32_t i = 0; i < num; i++) {
        cstr(rd(table + 8 * i), name, sizeof name);
        for (int k = 0; k < MAX_MODULES; k++) {
            const Loaded *ex = &mods[k];
            if (!ex->used || ex == importer || (only && ex != only))
                continue;
            uint32_t addr = export_named(ex, name);
            if (addr) {
                apply_import(importer, rd(table + 8 * i + 4), addr);
                break;
            }
        }
    }
    /* Indexed and anonymous imports, grouped by module. */
    uint32_t mt = hdr(importer, 0xF0), mnum = hdr(importer, 0xF4);
    for (uint32_t i = 0; i < mnum; i++) {
        uint32_t e = mt + 20 * i;
        cstr(rd(e), name, sizeof name);
        const Loaded *ex = strcmp(name, "|static|") == 0 ? NULL : find_loaded(name);
        if (strcmp(name, "|static|") == 0)
            for (int k = 0; k < MAX_MODULES; k++)
                if (mods[k].used && mods[k].is_static)
                    ex = &mods[k];
        if (!ex || ex == importer || (only && ex != only))
            continue;
        uint32_t idx = rd(e + 4), nidx = rd(e + 8), anon = rd(e + 12), nanon = rd(e + 16);
        for (uint32_t j = 0; j < nidx; j++) {
            uint32_t index = rd(idx + 8 * j);
            if (index >= hdr(ex, 0xDC)) {
                fprintf(stderr, "hle: RO: %s imports %s#%" PRIu32 ", which doesn't exist\n", importer->name,
                        ex->name, index);
                exit(3);
            }
            apply_import(importer, rd(idx + 8 * j + 4), seg_offset_addr(ex, rd(hdr(ex, 0xD8) + 4 * index)));
        }
        for (uint32_t j = 0; j < nanon; j++)
            apply_import(importer, rd(anon + 8 * j + 4), seg_offset_addr(ex, rd(anon + 8 * j)));
    }
}

/* Header tables hold file offsets; after loading they are addresses. */
static const uint32_t table_fields[] = {
    0x84, 0xC0, 0xC8, 0xD0, 0xD8, 0xE0, 0xE8, 0xF0, 0xF8, 0x100, 0x108, 0x110, 0x118, 0x120, 0x128, 0x130,
};

static void rebase_offsets(Loaded *m)
{
    for (size_t i = 0; i < sizeof table_fields / sizeof table_fields[0]; i++) {
        uint32_t v = rd(m->base + table_fields[i]);
        if (v)
            wr(m->base + table_fields[i], m->base + v);
    }
    /* Inside the tables, the name/string and patch-list pointers too. */
    uint32_t t = hdr(m, 0xD0);
    for (uint32_t i = 0; i < hdr(m, 0xD4); i++)
        wr(t + 8 * i, m->base + rd(t + 8 * i));
    t = hdr(m, 0x100);
    for (uint32_t i = 0; i < hdr(m, 0x104); i++) {
        wr(t + 8 * i, m->base + rd(t + 8 * i));
        wr(t + 8 * i + 4, m->base + rd(t + 8 * i + 4));
    }
    t = hdr(m, 0xF0);
    for (uint32_t i = 0; i < hdr(m, 0xF4); i++) {
        uint32_t e = t + 20 * i;
        wr(e, m->base + rd(e));
        wr(e + 4, m->base + rd(e + 4));
        wr(e + 12, m->base + rd(e + 12));
        for (uint32_t j = 0; j < rd(e + 8); j++)
            wr(rd(e + 4) + 8 * j + 4, m->base + rd(rd(e + 4) + 8 * j + 4));
        for (uint32_t j = 0; j < rd(e + 16); j++)
            wr(rd(e + 12) + 8 * j + 4, m->base + rd(rd(e + 12) + 8 * j + 4));
    }
}

static void read_segments(Loaded *m, uint32_t data_buf, uint32_t bss_buf)
{
    uint32_t table = m->base + hdr(m, 0xC8);        /* (before rebase_offsets: still an offset) */
    m->nsegs = (int)hdr(m, 0xCC);
    if (m->nsegs > 16) {
        fprintf(stderr, "hle: RO: %s: %d segments\n", m->name, m->nsegs);
        exit(3);
    }
    for (int i = 0; i < m->nsegs; i++) {
        uint32_t e = table + 12 * (uint32_t)i, off = rd(e), size = rd(e + 4), kind = rd(e + 8);
        uint32_t addr;
        if (m->is_static)
            addr = off;                         /* the CRS describes the main program: addresses */
        else if (kind == SEG_DATA) {
            addr = data_buf;
            memcpy(g(data_buf), g(m->base + off), size);
        } else if (kind == SEG_BSS) {
            addr = bss_buf;
            memset(g(bss_buf), 0, size);
        } else
            addr = m->base + off;
        if (!size && kind != SEG_TEXT)
            addr = 0;
        m->seg_addr[i] = addr;
        m->seg_size[i] = size;
        wr(e, addr);                            /* the segment table now holds addresses */
    }
}

static Loaded *new_module(void)
{
    for (int i = 0; i < MAX_MODULES; i++)
        if (!mods[i].used) {
            memset(&mods[i], 0, sizeof mods[i]);
            mods[i].used = 1;
            return &mods[i];
        }
    fprintf(stderr, "hle: RO: more than %d modules\n", MAX_MODULES);
    exit(3);
}

static void load(Loaded *m, uint32_t input, uint32_t mapped, uint32_t size, uint32_t data_buf,
                 uint32_t bss_buf, int fix_level)
{
    m->pristine = malloc(size);
    memcpy(m->pristine, g(input), size);
    if (input != mapped)
        memmove(g(mapped), g(input), size);     /* RO maps the heap buffer there */
    m->base = mapped;
    m->size = size;
    if (memcmp(g(mapped + 0x80), "CRO0", 4) != 0) {
        fprintf(stderr, "hle: RO: no CRO0 header at %08" PRIx32 "\n", mapped);
        exit(3);
    }
    uint32_t name_off = rd(mapped + 0xC0);
    cstr(mapped + name_off, m->name, sizeof m->name);
    read_segments(m, data_buf, bss_buf);
    rebase_offsets(m);

    /* Internal patches: the module's own pointers. */
    uint32_t t = hdr(m, 0x128);
    for (uint32_t i = 0; i < hdr(m, 0x12C); i++) {
        uint32_t e = t + 12 * i;
        apply(m, e, m->seg_addr[g(e)[5]]);
    }
    if (fix_level)
        memcpy(g(mapped + 0x80), "FIXD", 4);
    wr(mapped + 0x98, size);                    /* size after fixing: nothing dropped */
}

static void attach_code(Loaded *m)
{
    m->code = recomp_module_named(m->name);
    if (!m->code) {
        fprintf(stderr, "hle: RO: the game loaded module %s, which was not recompiled "
                        "(tools/build_dump.sh recompiles every .cro in the RomFS)\n", m->name);
        exit(3);
    }
    recomp_module_attach(m->code, m->base, m->size);
}

void ro_handle(KObj *session, uint32_t *cmd)
{
    (void)session;
    uint32_t id = cmd[0] >> 16;
    switch (id) {
    case 0x01: {    /* Initialize(CRS buffer, size, mapped address; process) */
        Loaded *m = new_module();
        m->is_static = 1;
        uint32_t input = cmd[1], size = cmd[2], mapped = cmd[3];
        if (input != mapped)
            memmove(g(mapped), g(input), size);
        m->base = mapped;
        m->size = size;
        snprintf(m->name, sizeof m->name, "|static|");
        read_segments(m, 0, 0);
        rebase_offsets(m);
        ipc_reply(cmd, RES_OK);
        return;
    }
    case 0x02:      /* LoadCRR(buffer, size; process): hashes we don't check */
    case 0x03:      /* UnloadCRR */
    case 0x08:      /* Shutdown */
        ipc_reply(cmd, RES_OK);
        return;
    case 0x04:      /* LoadCRO */
    case 0x09: {    /* LoadCRO_New: same arguments */
        uint32_t input = cmd[1], mapped = cmd[2], size = cmd[3], data_buf = cmd[4], bss_buf = cmd[7];
        int auto_link = cmd[9] != 0, fix_level = (int)cmd[10];
        Loaded *m = new_module();
        load(m, input, mapped, size, data_buf, bss_buf, fix_level);
        attach_code(m);
        if (auto_link) {
            link_imports(m, NULL);              /* its imports, from everything loaded */
            for (int i = 0; i < MAX_MODULES; i++)   /* and everyone's imports from it */
                if (mods[i].used && &mods[i] != m)
                    link_imports(&mods[i], m);
        }
        if (hle_trace)
            fprintf(stderr, "    -> RO loaded %s at %08" PRIx32 " (%" PRIu32 " bytes)\n", m->name, mapped, size);
        cmd[0] = ipc_header(id, 2, 0);
        cmd[1] = RES_OK;
        cmd[2] = size;
        return;
    }
    case 0x05: {    /* UnloadCRO(mapped address, ?, original address; process) */
        for (int i = 0; i < MAX_MODULES; i++)
            if (mods[i].used && !mods[i].is_static && mods[i].base == cmd[1]) {
                if (mods[i].code)
                    recomp_module_detach(mods[i].code);
                mods[i].used = 0;
                /* The game gets its buffer back as it was: unfixed. */
                memcpy(g(cmd[3]), mods[i].pristine, mods[i].size);
                free(mods[i].pristine);
                mods[i].pristine = NULL;
            }
        ipc_reply(cmd, RES_OK);
        return;
    }
    case 0x06:      /* LinkCRO(mapped address; process) */
        for (int i = 0; i < MAX_MODULES; i++)
            if (mods[i].used && mods[i].base == cmd[1]) {
                link_imports(&mods[i], NULL);
                for (int k = 0; k < MAX_MODULES; k++)
                    if (mods[k].used && k != i)
                        link_imports(&mods[k], &mods[i]);
            }
        ipc_reply(cmd, RES_OK);
        return;
    case 0x07:      /* UnlinkCRO(mapped address; process) */
        ipc_reply(cmd, RES_OK);
        return;
    default:
        ipc_unhandled("ldr:ro", cmd);
    }
}
