/* FS (fs:USER): files. Nothing is backed by real storage yet; this only
 * answers what libctru's startup asks, and reports "no SD card". */
#include <inttypes.h>
#include <stdio.h>

#include "service.h"

#define RES_FS_NOT_FOUND 0xC8804478u

void fs_handle(uint32_t *cmd)
{
    switch (cmd[0] >> 16) {
    case 0x0801:    /* Initialize(process id descriptor) */
    case 0x0861:    /* InitializeWithSdkVersion */
        ipc_reply(cmd, RES_OK);
        return;
    case 0x080C:    /* OpenArchive(id, path) */
        fprintf(stderr, "hle: FS OpenArchive(0x%" PRIx32 "): no storage in the HLE yet, returning not-found\n",
                cmd[1]);
        ipc_reply(cmd, RES_FS_NOT_FOUND);
        return;
    default:
        ipc_unhandled("fs:USER", cmd);
    }
}
