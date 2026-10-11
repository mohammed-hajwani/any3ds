/* HID (hid:USER): buttons, circle pad, touch screen, motion sensors.
 *
 * The app maps HID shared memory and reads it in hidScanInput. The pad
 * section (32-bit words):
 *   [0..1] tick of the latest update   [2..3] tick of the one before
 *   [4]    index (0-7) of the latest entry
 *   [10 + 4*i ..] entry i: held keys, newly pressed, newly released, circle pad
 * libctru ignores the section while both ticks are equal, so each update
 * writes a new tick. Touch (word 42), accelerometer (66) and gyro (86)
 * stay zero, which reads as "no data".
 *
 * Input comes from a script (hid_hold_keys): from a given frame on, hold
 * a given set of keys. */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "service.h"

#define MAX_SCRIPT 32

static KObj *shm, *pad_event;
static struct { uint32_t frame, keys; } script[MAX_SCRIPT];
static int nscript;
static uint32_t last_keys;

void hid_hold_keys(uint32_t frame, uint32_t keys)
{
    if (nscript == MAX_SCRIPT)
        return;
    script[nscript].frame = frame;
    script[nscript].keys = keys;
    nscript++;
}

static uint32_t keys_at(uint32_t frame)
{
    uint32_t keys = 0, from = 0;
    for (int i = 0; i < nscript; i++)
        if (script[i].frame <= frame && script[i].frame >= from) {
            from = script[i].frame;
            keys = script[i].keys;
        }
    return keys;
}

void hid_vblank(uint32_t frame)
{
    uint8_t *base = shm ? kshm_ptr(shm) : NULL;
    if (!base)
        return;
    uint32_t w[10];
    memcpy(w, base, sizeof w);
    uint32_t keys = keys_at(frame), index = (w[4] + 1) & 7;
    uint32_t entry[4] = { keys, keys & ~last_keys, last_keys & ~keys, 0 };
    if (keys != last_keys)
        fprintf(stderr, "    -> HID: frame %" PRIu32 ", keys %08" PRIx32 "\n", frame, keys);
    last_keys = keys;
    memcpy(base + (10 + 4 * index) * 4, entry, sizeof entry);
    uint64_t now = kernel_now();
    w[2] = w[0];                    /* previous tick */
    w[3] = w[1];
    w[0] = (uint32_t)now;
    w[1] = (uint32_t)(now >> 32);
    w[4] = index;
    w[7] = keys;                    /* raw current state */
    memcpy(base, w, sizeof w);
    kevent_signal(pad_event);
}

void hid_handle(uint32_t *cmd)
{
    switch (cmd[0] >> 16) {
    case 0x0A: {    /* GetIPCHandles -> shared memory, pad0, pad1, accel, gyro, debug pad events */
        static const char *names[5] = { "HID pad0", "HID pad1", "HID accel", "HID gyro", "HID debugpad" };
        if (!shm) {
            shm = kobj_new(KO_SHMEM, "HID shared mem");
            shm->shm.size = 0x2B0;
        }
        kernel_start_vblank();
        cmd[0] = ipc_header(0xA, 1, 7);
        cmd[1] = RES_OK;
        cmd[2] = IPC_COPY_HANDLES(6);
        cmd[3] = handle_new(shm);
        for (int i = 0; i < 5; i++) {
            KObj *ev = kevent_new(KRESET_ONESHOT, names[i]);
            if (i == 0)
                pad_event = ev;
            cmd[4 + i] = handle_new(ev);
        }
        return;
    }
    case 0x11:      /* EnableAccelerometer */
    case 0x12:      /* DisableAccelerometer */
    case 0x13:      /* EnableGyroscopeLow */
    case 0x14:      /* DisableGyroscopeLow */
        ipc_reply(cmd, RES_OK);
        return;
    default:
        ipc_unhandled("hid:USER", cmd);
    }
}
