/* GSP (gsp::Gpu): the graphics service. For now only the parts that put a
 * software-drawn framebuffer on screen; GPU command lists are not here.
 *
 * GSP shared memory (0x1000 bytes, mapped by the app):
 *   + thread * 0x40           interrupt queue: u8 first, u8 count, u8 error,
 *                             then (at +0xC) a ring of 0x34 event numbers
 *   + 0x200 + thread * 0x80   framebuffer info, top screen (+0x40: bottom):
 *                             u8 index, u8 update flag, then two entries of
 *                             { active, left addr, right addr, stride,
 *                               format, select, unused }
 *   + 0x800 + thread * 0x200  GX command queue (not handled yet)
 *
 * Every vblank we latch a newly presented framebuffer (if the app set the
 * update flag), push VBlank0/VBlank1 into the interrupt queue and signal
 * the app's event. libctru's GSP thread then wakes gspWaitForVBlank. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu.h"
#include "service.h"

#define EVENT_PSC0    0     /* memory fill 0 done */
#define EVENT_PSC1    1     /* memory fill 1 done */
#define EVENT_VBLANK0 2
#define EVENT_VBLANK1 3
#define EVENT_PPF     4     /* display transfer / texture copy done */
#define EVENT_P3D     5     /* command list done */
#define EVENT_DMA     6
#define QUEUE_LEN     0x34

typedef struct { uint32_t addr, stride, format; } Screen;

static KObj *shm, *interrupt_event;
static uint32_t thread_index;
static int lcd_black;
static Screen screens[2];
/* The last frame both screens showed, as the console looks: the 400x240
   top screen above the 320x240 bottom one, centred. */
#define FRAME_W 400
#define FRAME_H 480
static uint8_t frame_rgb[FRAME_W * FRAME_H * 3];
static uint8_t screen_rgb[400 * 240 * 3];
static int have_frame;

int fb_to_rgb(const uint8_t *fb, uint32_t format, uint32_t stride, int width, uint8_t *rgb);
int bmp_write(const char *path, const uint8_t *rgb, int width, int height);

static void push_interrupt(uint8_t *queue, uint8_t event)
{
    uint8_t first = queue[0], count = queue[1];
    if (count >= QUEUE_LEN) {
        queue[2] = 1;               /* overflow: the app missed events */
        return;
    }
    queue[0xC + (first + count) % QUEUE_LEN] = event;
    queue[1] = count + 1;
}

/* The app's gspPresentBuffer wrote a new entry and set the update flag. */
static void latch(uint8_t *base, int screen)
{
    uint8_t *header = base + 0x200 + thread_index * 0x80 + (uint32_t)screen * 0x40;
    if (!header[1])
        return;
    uint32_t info[7];
    memcpy(info, header + 4 + header[0] * 0x1C, sizeof info);
    screens[screen] = (Screen){ info[1], info[3], info[4] & 7 };
    header[1] = 0;
}

void gsp_vblank(void)
{
    uint8_t *base = shm ? kshm_ptr(shm) : NULL;
    if (!base)
        return;
    latch(base, 0);
    latch(base, 1);
    if (!lcd_black)
        for (int s = 0; s < 2; s++) {
            int width = s ? 320 : 400;
            if (!screens[s].addr || !fb_to_rgb(kernel_mem() + screens[s].addr, screens[s].format,
                                               screens[s].stride, width, screen_rgb))
                continue;
            for (int y = 0; y < 240; y++)
                memcpy(frame_rgb + ((size_t)(y + 240 * s) * FRAME_W + (s ? 40 : 0)) * 3,
                       screen_rgb + (size_t)y * (size_t)width * 3, (size_t)width * 3);
            have_frame = 1;
        }
    uint8_t *queue = base + thread_index * 0x40;
    push_interrupt(queue, EVENT_VBLANK0);
    push_interrupt(queue, EVENT_VBLANK1);
    kevent_signal(interrupt_event);
}

int gsp_dump_screens(const char *path)
{
    if (!have_frame) {
        fprintf(stderr, "hle: no frame was ever shown, nothing to dump\n");
        return 0;
    }
    if (!bmp_write(path, frame_rgb, FRAME_W, FRAME_H)) {
        perror(path);
        return 0;
    }
    fprintf(stderr, "hle: both screens (last frame shown) saved to %s\n", path);
    return 1;
}

/* The GX command queue (shared memory + 0x800 + thread * 0x200): u8 first,
   u8 count, then 15 slots of 8 words from word 8. Word 0's low byte is
   the command. The app only triggers the queue when it was empty, so each
   trigger runs everything queued; a finished command raises the interrupt
   the app waits for (gspWaitForPSC0, gspWaitForPPF, ...). */
static void run_gx_queue(void)
{
    uint8_t *base = kshm_ptr(shm), *q = base + 0x800 + thread_index * 0x200;
    uint8_t *interrupts = base + thread_index * 0x40;
    while (q[1]) {
        uint32_t c[8];
        memcpy(c, q + 0x20 * (1 + q[0]), sizeof c);
        q[0] = (uint8_t)((q[0] + 1) % 15);
        q[1]--;
        switch (c[0] & 0xFF) {
        case 0:     /* RequestDma(src, dst, size) */
            memcpy(gpu_mem(c[2], c[3]), gpu_mem(c[1], c[3]), c[3]);
            push_interrupt(interrupts, EVENT_DMA);
            break;
        case 2:     /* MemoryFill: two buffers, each (start, value, end) + a 16-bit control */
            gx_memory_fill(c[1], c[2], c[3], c[7] & 0xFFFF);
            gx_memory_fill(c[4], c[5], c[6], c[7] >> 16);
            if (c[1])
                push_interrupt(interrupts, EVENT_PSC0);
            if (c[4])
                push_interrupt(interrupts, EVENT_PSC1);
            break;
        case 3:     /* DisplayTransfer(in, out, in dim, out dim, flags) */
            gx_display_transfer(c[1], c[2], c[3], c[4], c[5]);
            push_interrupt(interrupts, EVENT_PPF);
            break;
        case 4:     /* TextureCopy(in, out, size, in dim, out dim, flags) */
            gx_texture_copy(c[1], c[2], c[3], c[4], c[5], c[6]);
            push_interrupt(interrupts, EVENT_PPF);
            break;
        case 5:     /* FlushCacheRegions: host memory is coherent */
            break;
        case 1:     /* ProcessCommandList(address, size, flags) */
            pica_run_command_list(c[1], c[2]);
            push_interrupt(interrupts, EVENT_P3D);
            break;
        default:
            gpu_fatal("GX command %" PRIu32, c[0] & 0xFF);
        }
    }
    kevent_signal(interrupt_event);
}

void gsp_handle(KObj *session, uint32_t *cmd)
{
    (void)session;
    switch (cmd[0] >> 16) {
    case 0x13: {    /* RegisterInterruptRelayQueue(flags, event) -> thread index, shared memory */
        interrupt_event = handle_get(cmd[3]);
        if (!interrupt_event || interrupt_event->type != KO_EVENT) {
            fprintf(stderr, "hle: GSP RegisterInterruptRelayQueue with a bad event handle\n");
            ipc_reply(cmd, RES_INVALID_HANDLE);
            return;
        }
        if (!shm) {
            shm = kobj_new(KO_SHMEM, "GSP shared mem");
            shm->shm.size = 0x1000;
        }
        kernel_start_vblank();
        cmd[0] = ipc_header(0x13, 2, 2);
        cmd[1] = RES_OK;            /* not 0x2A07: the HOME menu registered first and set up the LCDs */
        cmd[2] = thread_index;
        cmd[3] = IPC_COPY_HANDLES(1);
        cmd[4] = handle_new(shm);
        return;
    }
    case 0x0B:      /* SetLcdForceBlack(flag) */
        lcd_black = cmd[1] & 1;
        ipc_reply(cmd, RES_OK);
        return;
    case 0x01:      /* WriteHWRegs: LCD/GPU registers, no GPU here yet */
    case 0x02:      /* WriteHWRegsWithMask */
    case 0x08:      /* FlushDataCache: host memory is coherent */
    case 0x09:      /* InvalidateDataCache */
    case 0x14:      /* UnregisterInterruptRelayQueue */
    case 0x16:      /* AcquireRight */
    case 0x17:      /* ReleaseRight */
    case 0x19:      /* SaveVramSysArea */
    case 0x1A:      /* RestoreVramSysArea */
    case 0x1C:      /* SetLedForceOff */
        ipc_reply(cmd, RES_OK);
        return;
    case 0x0C:      /* TriggerCmdReqQueue: run the GX commands queued in shared memory */
        run_gx_queue();
        ipc_reply(cmd, RES_OK);
        return;
    default:
        ipc_unhandled("gsp::Gpu", cmd);
    }
}
