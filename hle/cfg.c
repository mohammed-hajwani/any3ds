/* CFG (cfg:u, cfg:s, cfg:i): the console's settings. An app reads
 * "config blocks" by id: language, country, user name, 3D/sound settings
 * and so on. We answer as an English-language European original 3DS (the
 * same model config_mem.c describes); unknown blocks read as zeros and
 * are logged once each, so a run shows which ones an app relies on. */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "service.h"

#define REGION_EUR      2
#define MODEL_3DS       0
#define LANGUAGE_EN     1

static void read_block(uint32_t id, uint8_t *out, uint32_t size)
{
    memset(out, 0, size);
    switch (id) {
    case 0x000A0002:            /* system language */
        out[0] = LANGUAGE_EN;
        return;
    case 0x000B0000:            /* country info: [2] = province, [3] = country (110 = UK) */
        if (size >= 4)
            out[3] = 110;
        return;
    case 0x00070001:            /* sound output: 1 = stereo */
        out[0] = 1;
        return;
    case 0x000B0001: {          /* country name, UTF-16, one 0x80-byte entry per language */
        static const char country[] = "United Kingdom";
        for (uint32_t lang = 0; (lang + 1) * 0x80 <= size; lang++)
            for (uint32_t i = 0; country[i]; i++)
                out[lang * 0x80 + 2 * i] = (uint8_t)country[i];
        return;
    }
    case 0x000B0002:            /* state name (UTF-16 per language): none */
    case 0x000B0003:            /* coordinates: 0, 0 */
        return;
    case 0x000A0000: {          /* user name: UTF-16, then flags */
        static const char name[] = "any3ds";
        for (uint32_t i = 0; name[i] && 2 * i + 1 < size; i++)
            out[2 * i] = (uint8_t)name[i];
        return;
    }
    default: {
        static uint32_t logged[32];
        static int nlogged;
        for (int i = 0; i < nlogged; i++)
            if (logged[i] == id)
                return;
        if (nlogged < 32)
            logged[nlogged++] = id;
        fprintf(stderr, "hle: CFG block 0x%08" PRIx32 " (%" PRIu32 " bytes) not modelled, reads as zeros\n", id, size);
    }
    }
}

static void reply_u8(uint32_t *cmd, uint32_t value)
{
    cmd[0] = ipc_header(cmd[0] >> 16, 2, 0);
    cmd[1] = RES_OK;
    cmd[2] = value;
}

void cfg_handle(KObj *session, uint32_t *cmd)
{
    (void)session;
    switch (cmd[0] >> 16) {
    case 0x0001:                /* GetConfigInfoBlk2(size, block id; buffer) */
    case 0x0401:                /* GetConfigInfoBlk4 (cfg:s) */
    case 0x0801: {              /* GetConfigInfoBlk8 (cfg:i) */
        uint32_t size = cmd[1], id = cmd[2], addr = cmd[4];
        read_block(id, kernel_mem() + addr, size);
        cmd[0] = ipc_header(cmd[0] >> 16, 1, 2);
        cmd[1] = RES_OK;
        cmd[2] = size << 4 | 0xC;
        cmd[3] = addr;
        return;
    }
    case 0x0002:                /* SecureInfoGetRegion */
        reply_u8(cmd, REGION_EUR);
        return;
    case 0x0003: {              /* GenHashConsoleUnique(salt) -> u64 */
        uint32_t salt = cmd[1];
        cmd[0] = ipc_header(0x3, 3, 0);
        cmd[1] = RES_OK;
        cmd[2] = 0x12345678u ^ salt;
        cmd[3] = 0;
        return;
    }
    case 0x0004:                /* GetRegionCanadaUSA */
    case 0x0006:                /* GetModelNintendo2DS: 0 = it is a 2DS... */
        reply_u8(cmd, (cmd[0] >> 16) == 0x0006 ? 1 : 0);    /* ...so 1: not a 2DS */
        return;
    case 0x0005:                /* GetSystemModel */
        reply_u8(cmd, MODEL_3DS);
        return;
    case 0x000B:                /* IsNFCSupported (a New 3DS feature) */
        reply_u8(cmd, 0);
        return;
    default:
        ipc_unhandled("cfg", cmd);
    }
}
