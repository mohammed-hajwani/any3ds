/* HID (hid:USER): buttons, circle pad, touch screen, motion sensors.
 *
 * The app maps HID shared memory and reads it in hidScanInput. Each input
 * has a section of 32-bit words laid out the same way:
 *   [0..1] tick of the latest update   [2..3] tick of the one before
 *   [4]    index (0-7) of the latest entry
 *   then a ring of 8 entries
 * libctru ignores a section while both ticks are equal, so each update
 * writes a new tick.
 *   pad    at word 0:  entries at word 10, 4 words each:
 *                      held keys, newly pressed, newly released, circle pad
 *   touch  at word 42: entries at word 50, 2 words each:
 *                      x | y << 16, touching (0/1)
 * Accelerometer (66) and gyro (86) stay zero, which reads as "no data".
 *
 * Input comes from a script: from a given frame on, hold these keys
 * (hid_hold_keys) / touch here or nowhere (hid_touch). It can also come
 * live from a file (hid_input_file), read every frame: the held keys in
 * hex, then optionally "X Y" for a touch on the bottom screen.
 * tools/live_server.py writes that file from buttons in tools/live.html. */
#define _POSIX_C_SOURCE 199309L     /* clock_gettime */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "service.h"

#define MAX_SCRIPT 4096
#define PAD_SECTION   0
#define TOUCH_SECTION 42

typedef struct { uint32_t frame, value, down; } Step;
typedef struct { Step steps[MAX_SCRIPT]; int n; } Script;

static KObj *shm, *pad_event;
static Script keys_script, touch_script;
static uint32_t last_keys, last_touch[2];
static const char *input_path;

static void script_add(Script *s, uint32_t frame, uint32_t value, uint32_t down)
{
    if (s->n == MAX_SCRIPT) {
        fprintf(stderr, "hle: more than %d scripted input changes\n", MAX_SCRIPT);
        exit(2);
    }
    s->steps[s->n++] = (Step){ frame, value, down };
}

/* The step in force at `frame`: the latest one that has started. */
static const Step *script_at(const Script *s, uint32_t frame)
{
    const Step *at = NULL;
    for (int i = 0; i < s->n; i++)
        if (s->steps[i].frame <= frame && (!at || s->steps[i].frame >= at->frame))
            at = &s->steps[i];
    return at;
}

void hid_hold_keys(uint32_t frame, uint32_t keys)
{
    script_add(&keys_script, frame, keys, 1);
}

void hid_touch(uint32_t frame, int down, uint32_t x, uint32_t y)
{
    script_add(&touch_script, frame, x | y << 16, (uint32_t)down);
}

void hid_input_file(const char *path)
{
    input_path = path;
}

/* Live input on top of the script: the file's keys are added to the held
   ones, and its touch (if any) wins. A missing file means nothing held. */
static void read_input_file(uint32_t *keys, uint32_t *touch)
{
    /* Read at most every 30 ms of real time (presses from the live view
       last at least 350 ms): opening a file on the Windows side from WSL
       every frame took a sixth of the run time. */
    static struct timespec last;
    static unsigned k, x, y;
    static int n;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if ((now.tv_sec - last.tv_sec) * 1000000000L + (now.tv_nsec - last.tv_nsec) >= 30000000L) {
        last = now;
        n = 0;
        FILE *f = fopen(input_path, "r");
        if (f) {
            n = fscanf(f, "%x %u %u", &k, &x, &y);
            fclose(f);
        }
    }
    if (n >= 1)
        *keys |= k;
    if (n == 3 && x < 320 && y < 240) {
        touch[0] = x | y << 16;
        touch[1] = 1;
    }
}

/* Publish a new entry in the section at word `section`, whose ring of
   entries starts at word `entries`. */
static void section_push(uint8_t *base, uint32_t section, uint32_t entries, const uint32_t *entry, uint32_t words)
{
    uint32_t w[5];
    memcpy(w, base + section * 4, sizeof w);
    uint32_t index = (w[4] + 1) & 7;
    memcpy(base + (entries + words * index) * 4, entry, words * 4);
    uint64_t now = kernel_now();
    w[2] = w[0];                    /* previous tick */
    w[3] = w[1];
    w[0] = (uint32_t)now;
    w[1] = (uint32_t)(now >> 32);
    w[4] = index;
    memcpy(base + section * 4, w, sizeof w);
}

void hid_vblank(uint32_t frame)
{
    uint8_t *base = shm ? kshm_ptr(shm) : NULL;
    if (!base)
        return;

    const Step *k = script_at(&keys_script, frame);
    uint32_t keys = k ? k->value : 0;
    const Step *t = script_at(&touch_script, frame);
    uint32_t touch[2] = { t && t->down ? t->value : 0, t ? t->down : 0 };
    if (input_path)
        read_input_file(&keys, touch);
    keys |= window_keys();              /* the window's keyboard and mouse (runner --window) */
    uint32_t wx, wy;
    if (window_touch(&wx, &wy)) {
        touch[0] = wx | wy << 16;
        touch[1] = 1;
    }

    uint32_t pad[4] = { keys, keys & ~last_keys, last_keys & ~keys, 0 };
    if (keys != last_keys)
        fprintf(stderr, "    -> HID: frame %" PRIu32 ", keys %08" PRIx32 "\n", frame, keys);
    last_keys = keys;
    section_push(base, PAD_SECTION, PAD_SECTION + 10, pad, 4);
    memcpy(base + 7 * 4, &keys, 4);     /* raw current pad state */

    if (touch[0] != last_touch[0] || touch[1] != last_touch[1])
        fprintf(stderr, "    -> HID: frame %" PRIu32 ", touch %s (%" PRIu32 ", %" PRIu32 ")\n", frame,
                touch[1] ? "down" : "up", touch[0] & 0xFFFF, touch[0] >> 16);
    memcpy(last_touch, touch, sizeof touch);
    section_push(base, TOUCH_SECTION, TOUCH_SECTION + 8, touch, 2);

    kevent_signal(pad_event);
}

void hid_handle(KObj *session, uint32_t *cmd)
{
    (void)session;
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
