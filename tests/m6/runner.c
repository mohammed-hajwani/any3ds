/* Run a recompiled libctru program on the HLE OS layer (hle/).
 *
 *   runner [--max-seconds N] [--hold FRAME:KEYS]... [--touch FRAME:X,Y|FRAME:up]...
 *          [--sdmc DIR] [--save DIR] [--romfs FILE] [--shared-font FILE] [--dump FILE.bmp]
 *          [--live FILE.bmp] [--input FILE]
 *
 * --hold presses buttons from a frame on (KEYS in hex, libctru's KEY_*
 * bits; 0 releases them), e.g. --hold 10:8 holds START from frame 10.
 * --touch touches the bottom screen at X,Y (or lifts the stylus) from a
 * frame on. --sdmc backs the SD card with a host folder, --save the app's
 * save data (formatted by the app the first time); --romfs gives the
 * app its RomFS (a RomFS image, or the .3dsx or decrypted .cxi/.3ds that
 * carries one). --shared-font gives it the system font: firmware we never
 * ship, so it must be your own dump of it (a .bcfnt).
 * --live saves both screens to FILE.bmp every 30 frames while it runs, to
 * watch it (tools/live.html shows that file, refreshing). --input reads
 * buttons live from FILE every frame: the held keys in hex, then
 * optionally "X Y" to touch the bottom screen (tools/live_server.py writes
 * it from the buttons on that page).
 * --dump saves the last frame shown when the program exits: the top
 * screen above the bottom one. Guest debug output (svcOutputDebugString) goes to stdout; the
 * OS layer's log goes to stderr (HLE_TRACE=1 logs every svc and IPC). */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel.h"

static const char *dump_path;

static void on_exit_dump(void)
{
    if (!dump_path)
        return;
    if (gsp_dump_screens(dump_path))
        fprintf(stderr, "hle: both screens (last frame shown) saved to %s\n", dump_path);
    else
        fprintf(stderr, "hle: no frame was ever shown, nothing to dump\n");
}

static void usage(void)
{
    fprintf(stderr, "usage: runner [--max-seconds N] [--hold FRAME:KEYS]... [--touch FRAME:X,Y|FRAME:up]...\n"
                    "              [--sdmc DIR] [--save DIR] [--romfs FILE] [--shared-font FILE] [--dump FILE.bmp]\n"
                    "              [--live FILE.bmp] [--input FILE]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    HleConfig config = { .max_seconds = 30, .on_exit = on_exit_dump };
    for (int i = 1; i < argc; i++) {
        if (i + 1 >= argc)
            usage();
        if (strcmp(argv[i], "--max-seconds") == 0) {
            config.max_seconds = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--hold") == 0) {
            char *colon;
            uint32_t frame = (uint32_t)strtoul(argv[++i], &colon, 10);
            if (*colon != ':')
                usage();
            hid_hold_keys(frame, (uint32_t)strtoul(colon + 1, NULL, 16));
        } else if (strcmp(argv[i], "--touch") == 0) {
            char *colon;
            uint32_t frame = (uint32_t)strtoul(argv[++i], &colon, 10), x = 0, y = 0;
            if (*colon != ':')
                usage();
            int down = strcmp(colon + 1, "up") != 0;
            if (down && sscanf(colon + 1, "%" SCNu32 ",%" SCNu32, &x, &y) != 2)
                usage();
            hid_touch(frame, down, x, y);
        } else if (strcmp(argv[i], "--sdmc") == 0) {
            fs_set_sdmc(argv[++i]);
        } else if (strcmp(argv[i], "--save") == 0) {
            fs_set_save(argv[++i]);
        } else if (strcmp(argv[i], "--shared-font") == 0) {
            if (!apt_set_shared_font(argv[++i]))
                return 1;
        } else if (strcmp(argv[i], "--romfs") == 0) {
            if (!fs_set_romfs(argv[++i])) {
                fprintf(stderr, "runner: can't read a RomFS from %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--live") == 0) {
            config.live_path = argv[++i];
            config.live_every = 30;
        } else if (strcmp(argv[i], "--input") == 0) {
            hid_input_file(argv[++i]);
        } else if (strcmp(argv[i], "--dump") == 0) {
            dump_path = argv[++i];
        } else {
            usage();
        }
    }

    uint8_t *mem = guest_mem_alloc();
    recomp_load_image(mem);
    config.entry = recomp_entry;
    if (!recomp_find(config.entry)) {
        fprintf(stderr, "no recompiled function at the entry point %08" PRIx32 "\n", config.entry);
        return 1;
    }
    hle_run(mem, &config);
}
