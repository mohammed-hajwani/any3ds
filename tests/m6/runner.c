/* Run a recompiled libctru program on the HLE OS layer (hle/).
 *
 *   runner [--max-seconds N] [--hold FRAME:KEYS]... [--dump FILE.bmp]
 *
 * --hold presses buttons from a frame on (KEYS in hex, libctru's KEY_*
 * bits; 0 releases them), e.g. --hold 10:8 holds START from frame 10.
 * --dump saves the last frame the top screen showed when the program
 * exits. Guest debug output (svcOutputDebugString) goes to stdout; the
 * OS layer's log goes to stderr (HLE_TRACE=1 logs every svc and IPC). */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel.h"

static const char *dump_path;

static void on_exit_dump(void)
{
    if (dump_path)
        gsp_dump_top(dump_path);
}

static void usage(void)
{
    fprintf(stderr, "usage: runner [--max-seconds N] [--hold FRAME:KEYS]... [--dump FILE.bmp]\n");
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
