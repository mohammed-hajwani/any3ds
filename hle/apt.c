/* APT: the applet manager. It tells the app when it may run (the "wakeup"
 * parameter), when HOME was pressed, when to sleep and when to close.
 *
 * libctru's aptInit: GetLockHandle -> Initialize (gets two events:
 * notification and parameter) -> starts its APT event thread -> Enable ->
 * waits until a WAKEUP parameter arrives. On a real 3DS the HOME menu sends
 * that parameter; here Enable queues it straight away. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "service.h"

#define APPID_HOMEMENU      0x101
#define APTCMD_WAKEUP       1
#define RES_NO_PARAMETER    0xC8A0CFEFu

static KObj *lock_mutex, *notify_event, *param_event;
static struct { int pending; uint32_t sender, command; } param;

static void send_parameter(uint32_t sender, uint32_t command)
{
    param.pending = 1;
    param.sender = sender;
    param.command = command;
    kevent_signal(param_event);
}

/* ReceiveParameter (0xD) and GlanceParameter (0xE): the same reply; only
   Receive takes the parameter off the queue. */
static void parameter(uint32_t *cmd, int receive)
{
    uint32_t id = cmd[0] >> 16;
    if (!param.pending) {
        ipc_reply(cmd, RES_NO_PARAMETER);
        return;
    }
    cmd[0] = ipc_header(id, 4, 4);
    cmd[1] = RES_OK;
    cmd[2] = param.sender;
    cmd[3] = param.command;
    cmd[4] = 0;                             /* size of the parameter data */
    cmd[5] = IPC_MOVE_HANDLES(1);
    cmd[6] = 0;                             /* no handle travels with it */
    cmd[7] = IPC_STATIC_BUFFER(0, 0);
    cmd[8] = ipc_static_buffer(0, NULL);
    if (hle_trace || receive)
        fprintf(stderr, "    -> APT parameter: command %" PRIu32 " from applet %03" PRIx32 "%s\n",
                param.command, param.sender, receive ? "" : " (glance)");
    if (receive)
        param.pending = 0;
}

/* ---- The system font
 *
 * It is console firmware: we never ship it, but you can give the runner
 * your own dump of it (--shared-font cbf_std.bcfnt). On a console, NS
 * loads it at boot into a 0x332000-byte shared memory block (3dbrew:
 * System_Font): a 0x80-byte header (status 2 = loaded, region 1 =
 * JPN/EUR/USA, the font's size), then the font, whose magic NS changes
 * from "CFNT" to "CFNU" and whose file offsets it turns into addresses.
 * GetSharedFont hands out that block and the address it is mapped at. */
#define FONT_BLOCK_SIZE 0x332000u
#define FONT_ADDR       0x18000000u     /* the start of the shared memory region */

static KObj *font_block;

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

/* Offset -> address in the font at `font` (an offset of 0 ends a list). */
static int relocate(uint8_t *font, uint32_t size, uint32_t at, uint32_t base)
{
    if (at + 4 > size)
        return 0;
    uint32_t off = rd32(font + at);
    if (!off)
        return 1;
    if (off >= size)
        return 0;
    uint32_t addr = base + off;
    memcpy(font + at, &addr, 4);
    return 1;
}

int apt_set_shared_font(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    uint8_t *block = calloc(1, FONT_BLOCK_SIZE), *font = block + 0x80;
    size_t size = block ? fread(font, 1, FONT_BLOCK_SIZE - 0x80, f) : 0;
    int more = fgetc(f) != EOF;
    fclose(f);
    if (!block || more || size < 0x30 || memcmp(font, "CFNT", 4) != 0 || rd16(font + 4) != 0xFEFF) {
        fprintf(stderr, "hle: %s: not an uncompressed system font (.bcfnt, \"CFNT\", at most %#x bytes)\n",
                path, FONT_BLOCK_SIZE - 0x80);
        free(block);
        return 0;
    }
    uint32_t n = (uint32_t)size, base = FONT_ADDR + 0x80;
    /* The FINF section follows the 0x14-byte header; its tglp, cwdh and
       cmap pointers are at 0x24, 0x28 and 0x2C; each points just past its
       section's 8-byte header. */
    uint32_t tglp = rd32(font + 0x24), cwdh = rd32(font + 0x28), cmap = rd32(font + 0x2C);
    int ok = memcmp(font + 0x14, "FINF", 4) == 0
        && relocate(font, n, 0x24, base) && relocate(font, n, 0x28, base) && relocate(font, n, 0x2C, base)
        && tglp && relocate(font, n, tglp + 20, base);              /* the glyph sheets */
    for (int i = 0; ok && cwdh; i++) {                              /* glyph widths: a list */
        uint32_t next = cwdh + 4 + 4 <= n ? rd32(font + cwdh + 4) : 0;
        ok = i < 4096 && relocate(font, n, cwdh + 4, base);
        cwdh = next;
    }
    for (int i = 0; ok && cmap; i++) {                              /* code -> glyph maps: a list */
        uint32_t next = cmap + 8 + 4 <= n ? rd32(font + cmap + 8) : 0;
        ok = i < 4096 && relocate(font, n, cmap + 8, base);
        cmap = next;
    }
    if (!ok) {
        fprintf(stderr, "hle: %s: a font section points outside the file\n", path);
        free(block);
        return 0;
    }
    memcpy(font, "CFNU", 4);
    uint32_t header[3] = { 2, 1, n };   /* status: loaded; region: JPN/EUR/USA; size */
    memcpy(block, header, sizeof header);

    font_block = kobj_new(KO_SHMEM, "shared font");
    font_block->shm.size = FONT_BLOCK_SIZE;
    font_block->shm.home = FONT_ADDR;
    font_block->shm.init = block;
    return 1;
}

void apt_handle(KObj *session, uint32_t *cmd)
{
    (void)session;
    switch (cmd[0] >> 16) {
    case 0x01:      /* GetLockHandle(flags) -> applet attributes, power state, lock mutex */
        if (!lock_mutex)
            lock_mutex = kobj_new(KO_MUTEX, "APT lock");
        cmd[0] = ipc_header(0x1, 3, 2);
        cmd[1] = RES_OK;
        cmd[2] = 0;
        cmd[3] = 0;
        cmd[4] = IPC_COPY_HANDLES(1);
        cmd[5] = handle_new(lock_mutex);
        return;
    case 0x02:      /* Initialize(app id, attributes) -> notification + parameter events */
        notify_event = kevent_new(KRESET_ONESHOT, "APT notification");
        param_event = kevent_new(KRESET_ONESHOT, "APT parameter");
        cmd[0] = ipc_header(0x2, 1, 3);
        cmd[1] = RES_OK;
        cmd[2] = IPC_COPY_HANDLES(2);
        cmd[3] = handle_new(notify_event);
        cmd[4] = handle_new(param_event);
        return;
    case 0x03:      /* Enable(attributes): HOME menu answers with WAKEUP */
        ipc_reply(cmd, RES_OK);
        send_parameter(APPID_HOMEMENU, APTCMD_WAKEUP);
        return;
    case 0x06:      /* GetAppletInfo(app id) -> title id, media type, registered, loaded, attributes */
        cmd[0] = ipc_header(0x6, 7, 0);
        cmd[1] = RES_OK;
        cmd[2] = cmd[3] = 0;
        cmd[4] = 0;
        cmd[5] = 1;
        cmd[6] = 1;
        cmd[7] = 0;
        return;
    case 0x0B:      /* InquireNotification(app id) -> signal (0 = none) */
        cmd[0] = ipc_header(0xB, 2, 0);
        cmd[1] = RES_OK;
        cmd[2] = 0;
        return;
    case 0x0D:
        parameter(cmd, 1);
        return;
    case 0x0E:
        parameter(cmd, 0);
        return;
    case 0x04:      /* Finalize */
    case 0x22:      /* PrepareToCloseApplication */
    case 0x27:      /* CloseApplication */
    case 0x43:      /* NotifyToWait */
    case 0x4F:      /* SetAppCpuTimeLimit */
        ipc_reply(cmd, RES_OK);
        return;
    case 0x4B:      /* AppletUtility(id, ...) -> the utility's own result */
        cmd[0] = ipc_header(0x4B, 2, 0);
        cmd[1] = RES_OK;
        cmd[2] = RES_OK;
        return;
    case 0x44:      /* GetSharedFont -> map address, shared memory handle */
        if (!font_block) {
            fprintf(stderr, "hle: APT GetSharedFont: the system font is firmware, which we never ship; "
                            "give the runner your own dump with --shared-font FILE.bcfnt\n");
            exit(3);
        }
        cmd[0] = ipc_header(0x44, 2, 2);
        cmd[1] = RES_OK;
        cmd[2] = FONT_ADDR;
        cmd[3] = IPC_COPY_HANDLES(1);
        cmd[4] = handle_new(font_block);
        return;
    case 0x102:     /* CheckNew3DS -> 0: an original 3DS (no C-stick/ZL/ZR, 268 MHz) */
        cmd[0] = ipc_header(0x102, 2, 0);
        cmd[1] = RES_OK;
        cmd[2] = 0;
        return;
    case 0x50:      /* GetAppCpuTimeLimit -> percent */
        cmd[0] = ipc_header(0x50, 2, 0);
        cmd[1] = RES_OK;
        cmd[2] = 0;
        return;
    default:
        ipc_unhandled("APT", cmd);
    }
}
