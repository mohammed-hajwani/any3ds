/* y2r:u, the YUV-to-RGB converter: a hardware block that an app feeds
 * with YUV (usually frames from its video decoder) and reads RGB back
 * from, both by DMA. Sources: 3dbrew's Camera_Services page (the Y2R
 * commands and enums) and libctru's y2r.h, which documents the formats
 * and the conversion formula:
 *
 *   R = trunc(rgb_Y * Y           + r_V * V + 0.75 + r_offset)
 *   G = trunc(rgb_Y * Y - g_U * U - g_V * V + 0.75 + g_offset)
 *   B = trunc(rgb_Y * Y + b_U * U           + 0.75 + b_offset)
 *
 * Y, U, V are the raw 8-bit samples; rgb_Y .. b_U are unsigned 2.8 fixed
 * point and the offsets signed 11.5 fixed point; results clamp to 0..255.
 *
 * - Each input (planes Y, U, V, or packed YUYV) and the output move in
 *   units of `transfer_unit` bytes with `transfer_gap` bytes skipped after
 *   each, `image_size` bytes in all (so an app can convert part of a
 *   wider buffer).
 * - The block converts 8 lines at a time. With BLOCK_8_BY_8, each batch of
 *   8 lines comes out as 8x8 tiles in Morton order: the GPU's texture
 *   layout. The output formats are GPU color formats, but numbered
 *   differently: Y2R's 2 is RGBA5551 and 3 is RGB565.
 * - We convert the whole image when the app starts the conversion, then
 *   raise the transfer-end event (if the app asked for that interrupt).
 *
 * Not modelled: rotation (a hard stop if used) and dithering (it only
 * changes the low bits of 16-bit output). */
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu.h"
#include "service.h"

enum { YUV422_INDIV_8, YUV420_INDIV_8, YUV422_INDIV_16, YUV420_INDIV_16, YUV422_BATCH };
enum { BLOCK_LINE, BLOCK_8_BY_8 };

/* One DMA stream: image_size bytes, moved transfer_unit bytes at a time
   with transfer_gap bytes skipped after each. */
typedef struct {
    uint32_t addr, size;
    int16_t unit, gap;
    int done;
} Transfer;

static struct {
    uint32_t input_format, output_format, rotation, alignment;
    uint32_t spatial_dither, temporal_dither, end_interrupt;
    uint32_t line_width, lines, alpha;
    uint16_t coef[8];               /* rgb_Y, r_V, g_V, g_U, b_U, r/g/b offsets */
    uint16_t dither[16];
    Transfer y, u, v, yuyv, out;
} st;

static KObj *end_event;

/* The standard presets, from the luma weights (Kr, Kb) of ITU-R BT.601
   and BT.709, either full range or scaled from TV range (Y 16-235, chroma
   16-240), rounded down into the fixed-point formats above. */
static void standard_coefficients(uint32_t which, uint16_t c[8])
{
    double kr = which & 1 ? 0.2126 : 0.299, kb = which & 1 ? 0.0722 : 0.114, kg = 1 - kr - kb;
    double ys = which & 2 ? 255.0 / 219 : 1, cs = which & 2 ? 255.0 / 224 : 1, y0 = which & 2 ? 16 : 0;
    double rv = 2 * (1 - kr) * cs, bu = 2 * (1 - kb) * cs;
    double gu = 2 * kb * (1 - kb) / kg * cs, gv = 2 * kr * (1 - kr) / kg * cs;
    double m[5] = { ys, rv, gv, gu, bu };
    double off[3] = { -ys * y0 - rv * 128, -ys * y0 + (gu + gv) * 128, -ys * y0 - bu * 128 };
    for (int i = 0; i < 5; i++)
        c[i] = (uint16_t)floor(m[i] * 256);
    for (int i = 0; i < 3; i++)
        c[5 + i] = (uint16_t)(int16_t)floor(off[i] * 32);
}

static void reset(void)
{
    memset(&st, 0, sizeof st);
    standard_coefficients(0, st.coef);
}

static uint32_t transfer_step(const Transfer *t)
{
    return t->unit > 0 ? (uint32_t)t->unit : t->size;
}

/* A stream's bytes, out of guest memory into one contiguous buffer. */
static uint8_t *gather(const Transfer *t)
{
    uint8_t *buf = calloc(1, t->size + 1);
    uint32_t a = t->addr, unit = transfer_step(t);
    for (uint32_t done = 0; buf && done < t->size;) {
        uint32_t n = t->size - done < unit ? t->size - done : unit;
        memcpy(buf + done, kernel_mem() + a, n);
        done += n;
        a += n + (uint32_t)(int32_t)t->gap;
    }
    return buf;
}

/* The reverse: a contiguous buffer out to guest memory, unit by unit. */
static void scatter(const Transfer *t, const uint8_t *buf, uint32_t size)
{
    uint32_t a = t->addr, unit = transfer_step(t);
    if (size > t->size)
        size = t->size;
    for (uint32_t done = 0; done < size;) {
        uint32_t n = size - done < unit ? size - done : unit;
        memcpy(kernel_mem() + a, buf + done, n);
        done += n;
        a += n + (uint32_t)(int32_t)t->gap;
    }
}

/* Sample i of a plane: one byte, or the low byte of two (16-bit formats). */
static int sample(const uint8_t *plane, uint32_t plane_size, uint32_t i, int wide)
{
    uint32_t at = wide ? 2 * i : i;
    return at < plane_size ? plane[at] : 0;
}

static int clamp8(int32_t acc)
{
    acc >>= 8;                      /* 1/256 units -> whole values */
    return acc < 0 ? 0 : acc > 255 ? 255 : (int)acc;
}

/* Y2R output format -> GPU color format. */
static const int out_formats[4] = { PIXEL_RGBA8, PIXEL_RGB8, PIXEL_RGB5A1, PIXEL_RGB565 };

static void convert(void)
{
    uint32_t w = st.line_width, h = st.lines, fmt = st.input_format;
    int pixel = st.output_format < 4 ? out_formats[st.output_format] : -1;
    int bpp = pixel < 0 ? 0 : pixel_size(pixel);
    if (st.rotation != 0) {
        fprintf(stderr, "hle: Y2R rotation %" PRIu32 " is not modelled\n", st.rotation);
        exit(3);
    }
    if (fmt > YUV422_BATCH || !bpp || !w || !h) {
        fprintf(stderr, "hle: Y2R conversion with input format %" PRIu32 ", output format %" PRIu32
                ", %" PRIu32 "x%" PRIu32 " is not supported\n", fmt, st.output_format, w, h);
        exit(3);
    }
    int wide = fmt == YUV422_INDIV_16 || fmt == YUV420_INDIV_16;
    int batch = fmt == YUV422_BATCH;
    uint8_t *ys = gather(batch ? &st.yuyv : &st.y);
    uint8_t *us = batch ? NULL : gather(&st.u), *vs = batch ? NULL : gather(&st.v);
    uint32_t ysize = batch ? st.yuyv.size : st.y.size;
    uint32_t out_size = w * h * (uint32_t)bpp;
    uint8_t *out = calloc(1, out_size);
    if (!ys || (!batch && (!us || !vs)) || !out) {
        perror("y2r");
        exit(1);
    }
    const uint16_t *c = st.coef;
    int32_t ro = (int16_t)c[5] * 8, go = (int16_t)c[6] * 8, bo = (int16_t)c[7] * 8;    /* 11.5 -> 1/256 */
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            int Y, U, V;
            if (batch) {            /* Y0 U Y1 V */
                uint32_t pair = (y * w + x) & ~1u;
                Y = sample(ys, ysize, 2 * (y * w + x), 0);
                U = sample(ys, ysize, 2 * pair + 1, 0);
                V = sample(ys, ysize, 2 * pair + 3, 0);
            } else {
                uint32_t row = fmt == YUV420_INDIV_8 || fmt == YUV420_INDIV_16 ? y / 2 : y;
                uint32_t ci = row * (w / 2) + x / 2;
                Y = sample(ys, ysize, y * w + x, wide);
                U = sample(us, st.u.size, ci, wide);
                V = sample(vs, st.v.size, ci, wide);
            }
            int32_t yy = c[0] * Y + 192;                    /* + 0.75 */
            Rgba px = { (uint8_t)clamp8(yy + c[1] * V + ro),
                        (uint8_t)clamp8(yy - c[3] * U - c[2] * V + go),
                        (uint8_t)clamp8(yy + c[4] * U + bo),
                        (uint8_t)st.alpha };
            uint32_t i = st.alignment == BLOCK_8_BY_8 ? (y / 8) * (w * 8) + tile_index(x, y % 8, w)
                                                      : y * w + x;
            color_encode(out + i * (uint32_t)bpp, pixel, px);
        }
    scatter(&st.out, out, out_size);
    free(ys);
    free(us);
    free(vs);
    free(out);
}

static void set_transfer(Transfer *t, const uint32_t *cmd)
{
    *t = (Transfer){ cmd[1], cmd[2], (int16_t)cmd[3], (int16_t)cmd[4], 0 };
}

static void reply_value(uint32_t *cmd, uint32_t value)
{
    cmd[0] = ipc_header(cmd[0] >> 16, 2, 0);
    cmd[1] = RES_OK;
    cmd[2] = value;
}

/* The settings SetPackageParameter takes: input, output, rotation and
   alignment bytes; line width and lines; standard coefficient; alpha. */
static void set_package(const uint32_t *w)
{
    st.input_format = w[0] & 0xFF;
    st.output_format = (w[0] >> 8) & 0xFF;
    st.rotation = (w[0] >> 16) & 0xFF;
    st.alignment = w[0] >> 24;
    st.line_width = w[1] & 0xFFFF;
    st.lines = w[1] >> 16;
    standard_coefficients(w[2] & 0xFF, st.coef);
    st.alpha = w[2] >> 16;
}

void y2r_handle(KObj *session, uint32_t *cmd)
{
    (void)session;
    static int initialised;
    if (!initialised) {
        reset();
        initialised = 1;
    }
    uint32_t id = cmd[0] >> 16;
    /* Setter/getter pairs of one value each: (set id, field). */
    static const struct { uint32_t set; uint32_t *field; } values[] = {
        { 0x01, &st.input_format }, { 0x03, &st.output_format }, { 0x05, &st.rotation },
        { 0x07, &st.alignment }, { 0x09, &st.spatial_dither }, { 0x0B, &st.temporal_dither },
        { 0x0D, &st.end_interrupt }, { 0x1A, &st.line_width }, { 0x1C, &st.lines }, { 0x22, &st.alpha },
    };
    for (size_t i = 0; i < sizeof values / sizeof *values; i++) {
        if (id == values[i].set) {
            *values[i].field = cmd[1] & (id == 0x1A || id == 0x1C || id == 0x22 ? 0xFFFF : 0xFF);
            ipc_reply(cmd, RES_OK);
            return;
        }
        if (id == values[i].set + 1) {
            reply_value(cmd, *values[i].field);
            return;
        }
    }
    switch (id) {
    case 0x0F:      /* GetTransferEndEvent */
        if (!end_event)
            end_event = kevent_new(KRESET_ONESHOT, "Y2R transfer end");
        cmd[0] = ipc_header(0x0F, 1, 2);
        cmd[1] = RES_OK;
        cmd[2] = IPC_COPY_HANDLES(1);
        cmd[3] = handle_new(end_event);
        return;
    case 0x10: set_transfer(&st.y, cmd); ipc_reply(cmd, RES_OK); return;     /* SetSendingY(addr, size, unit, gap) */
    case 0x11: set_transfer(&st.u, cmd); ipc_reply(cmd, RES_OK); return;     /* SetSendingU */
    case 0x12: set_transfer(&st.v, cmd); ipc_reply(cmd, RES_OK); return;     /* SetSendingV */
    case 0x13: set_transfer(&st.yuyv, cmd); ipc_reply(cmd, RES_OK); return;  /* SetSendingYUYV */
    case 0x18: set_transfer(&st.out, cmd); ipc_reply(cmd, RES_OK); return;   /* SetReceiving */
    case 0x14: reply_value(cmd, (uint32_t)st.yuyv.done); return;            /* IsDoneSendingYUYV */
    case 0x15: reply_value(cmd, (uint32_t)st.y.done); return;               /* IsDoneSendingY */
    case 0x16: reply_value(cmd, (uint32_t)st.u.done); return;               /* IsDoneSendingU */
    case 0x17: reply_value(cmd, (uint32_t)st.v.done); return;               /* IsDoneSendingV */
    case 0x19: reply_value(cmd, (uint32_t)st.out.done); return;             /* IsDoneReceiving */
    case 0x1E:      /* SetCoefficients(8 x u16) */
        memcpy(st.coef, cmd + 1, sizeof st.coef);
        ipc_reply(cmd, RES_OK);
        return;
    case 0x1F:      /* GetCoefficients */
        cmd[0] = ipc_header(0x1F, 5, 0);
        cmd[1] = RES_OK;
        memcpy(cmd + 2, st.coef, sizeof st.coef);
        return;
    case 0x20:      /* SetStandardCoefficient(which) */
        standard_coefficients(cmd[1] & 3, st.coef);
        ipc_reply(cmd, RES_OK);
        return;
    case 0x21: {    /* GetStandardCoefficient(which) -> its 8 coefficients */
        uint16_t c[8];
        standard_coefficients(cmd[1] & 3, c);
        cmd[0] = ipc_header(0x21, 5, 0);
        cmd[1] = RES_OK;
        memcpy(cmd + 2, c, sizeof c);
        return;
    }
    case 0x24:      /* SetDitheringWeightParams(16 x u16) */
        memcpy(st.dither, cmd + 1, sizeof st.dither);
        ipc_reply(cmd, RES_OK);
        return;
    case 0x25:      /* GetDitheringWeightParams */
        cmd[0] = ipc_header(0x25, 9, 0);
        cmd[1] = RES_OK;
        memcpy(cmd + 2, st.dither, sizeof st.dither);
        return;
    case 0x26:      /* StartConversion: done at once */
        convert();
        st.y.done = st.u.done = st.v.done = st.yuyv.done = st.out.done = 1;
        if (st.end_interrupt && end_event)
            kevent_signal(end_event);
        ipc_reply(cmd, RES_OK);
        return;
    case 0x27:      /* StopConversion */
        ipc_reply(cmd, RES_OK);
        return;
    case 0x28:      /* IsBusyConversion */
        reply_value(cmd, 0);
        return;
    case 0x29:      /* SetPackageParameter(12 bytes) */
        set_package(cmd + 1);
        ipc_reply(cmd, RES_OK);
        return;
    case 0x2A:      /* PingProcess */
        reply_value(cmd, 0);
        return;
    case 0x2B:      /* DriverInitialize */
        reset();
        ipc_reply(cmd, RES_OK);
        return;
    case 0x2C:      /* DriverFinalize */
        ipc_reply(cmd, RES_OK);
        return;
    default:
        ipc_unhandled("y2r:u", cmd);
    }
}
