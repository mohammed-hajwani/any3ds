/* Y2R test: libctru's y2r client against our y2r:u (hle/y2r.c). The
 * checks follow from how the hardware is documented to behave (libctru's
 * y2r.h, 3dbrew's Camera_Services), computed here independently:
 *   - every pixel matches the documented formula, evaluated in floating
 *     point from the coefficients the service reports;
 *   - the standard presets behave like their standards: mid grey stays
 *     grey, TV-range black and white become 0 and 255;
 *   - transfers honour transfer_unit / transfer_gap (a plane inside a
 *     wider buffer);
 *   - BLOCK_8_BY_8 output is 8x8 tiles in Morton order, x bit first;
 *   - the 16-bit input formats use the low byte of each sample;
 *   - the transfer-end event fires and IsDoneReceiving says so.
 *
 * Output goes to stderr (svcOutputDebugString) and must match
 * y2r_test.expected. */
#include <3ds.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define say(...) fprintf(stderr, __VA_ARGS__)

#define W 16
#define H 8

static Handle end_event;

/* The documented formula, in floating point. ch: 0 = R, 1 = G, 2 = B. */
static int expect(const Y2RU_ColorCoefficients *c, int y, int u, int v, int ch)
{
    double Y = c->rgb_Y / 256.0 * y, x;
    if (ch == 0)
        x = Y + c->r_V / 256.0 * v + (s16)c->r_offset / 32.0;
    else if (ch == 1)
        x = Y - c->g_U / 256.0 * u - c->g_V / 256.0 * v + (s16)c->g_offset / 32.0;
    else
        x = Y + c->b_U / 256.0 * u + (s16)c->b_offset / 32.0;
    x = floor(x + 0.75);
    return x < 0 ? 0 : x > 255 ? 255 : (int)x;
}

/* Index of pixel (x, y) in BLOCK_8_BY_8 output: 8-line batches of 8x8
   tiles left to right, Morton order inside a tile (x bit first). */
static int tiled(int x, int y)
{
    int m = 0;
    for (int b = 0; b < 3; b++)
        m |= ((x >> b) & 1) << (2 * b) | ((y >> b) & 1) << (2 * b + 1);
    return (y / 8) * (W * 8) + (x / 8) * 64 + m;
}

static int convert(void)
{
    Y2RU_SetTransferEndInterrupt(true);
    Y2RU_StartConversion();
    if (svcWaitSynchronization(end_event, 1000000000LL) != 0) {
        say("FAIL: no transfer-end event\n");
        return 0;
    }
    bool done = false;
    Y2RU_IsDoneReceiving(&done);
    if (!done) {
        say("FAIL: IsDoneReceiving says no\n");
        return 0;
    }
    return 1;
}

static void setup(Y2RU_InputFormat in, Y2RU_OutputFormat out, Y2RU_BlockAlignment align, u16 alpha)
{
    Y2RU_ConversionParams p = { in, out, ROTATION_NONE, align, W, H, COEFFICIENT_ITU_R_BT_601, 0, alpha };
    Y2RU_SetConversionParams(&p);
}

/* Planar 4:2:0, 8-bit, Y inside a wider buffer (24 bytes per line: 16 of
   image, 8 skipped) -> RGBA8888, linear. */
static void test_420_rgba(void)
{
    static u8 ybuf[24 * H], ubuf[(W / 2) * (H / 2)], vbuf[(W / 2) * (H / 2)];
    static u32 out[W * H];
    for (int y = 0; y < H; y++)
        for (int x = 0; x < 24; x++)
            ybuf[y * 24 + x] = x < W ? (u8)(16 * x + 7 * y) : 0xEE;
    for (int i = 0; i < (W / 2) * (H / 2); i++) {
        ubuf[i] = (u8)(40 + 13 * i);
        vbuf[i] = (u8)(220 - 11 * i);
    }
    setup(INPUT_YUV420_INDIV_8, OUTPUT_RGB_32, BLOCK_LINE, 0x80);
    Y2RU_SetSendingY(ybuf, W * H, W, 24 - W);
    Y2RU_SetSendingU(ubuf, sizeof ubuf, sizeof ubuf, 0);
    Y2RU_SetSendingV(vbuf, sizeof vbuf, sizeof vbuf, 0);
    memset(out, 0, sizeof out);
    Y2RU_SetReceiving(out, sizeof out, sizeof out, 0);
    if (!convert())
        return;
    Y2RU_ColorCoefficients c;
    Y2RU_GetStandardCoefficient(&c, COEFFICIENT_ITU_R_BT_601);
    int bad = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int Y = ybuf[y * 24 + x], ci = (y / 2) * (W / 2) + x / 2, U = ubuf[ci], V = vbuf[ci];
            u32 want = (u32)expect(&c, Y, U, V, 0) << 24 | (u32)expect(&c, Y, U, V, 1) << 16
                     | (u32)expect(&c, Y, U, V, 2) << 8 | 0x80;
            if (out[y * W + x] != want && bad++ < 3)
                say("  pixel (%d, %d): %08lx, expected %08lx\n", x, y, out[y * W + x], want);
        }
    say("%s: 4:2:0 planes (Y with a gap) -> RGBA8888, every pixel by the formula\n", bad ? "FAIL" : "PASS");
}

/* Packed YUYV -> RGB565 in 8x8 tiles. */
static void test_yuyv_565_tiled(void)
{
    static u8 in[W * H * 2];
    static u16 out[W * H];
    for (int i = 0; i < W * H; i++) {
        in[2 * i] = (u8)(3 * i);                            /* Y */
        in[2 * i + 1] = (u8)((i & 1) ? 200 - i : 60 + i);   /* U on even, V on odd */
    }
    setup(INPUT_YUV422_BATCH, OUTPUT_RGB_16_565, BLOCK_8_BY_8, 0xFF);
    Y2RU_SetSendingYUYV(in, sizeof in, sizeof in, 0);
    memset(out, 0, sizeof out);
    Y2RU_SetReceiving(out, sizeof out, sizeof out, 0);
    if (!convert())
        return;
    Y2RU_ColorCoefficients c;
    Y2RU_GetStandardCoefficient(&c, COEFFICIENT_ITU_R_BT_601);
    int bad = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int i = y * W + x, pair = i & ~1;
            int Y = in[2 * i], U = in[2 * pair + 1], V = in[2 * pair + 3];
            u16 want = (u16)((expect(&c, Y, U, V, 0) >> 3) << 11 | (expect(&c, Y, U, V, 1) >> 2) << 5
                             | expect(&c, Y, U, V, 2) >> 3);
            if (out[tiled(x, y)] != want && bad++ < 3)
                say("  pixel (%d, %d): %04x, expected %04x\n", x, y, out[tiled(x, y)], want);
        }
    say("%s: packed YUYV -> RGB565 in 8x8 Morton tiles\n", bad ? "FAIL" : "PASS");
}

/* Planar 4:2:2 with 16-bit samples (value in the low byte) -> RGB888. */
static void test_422_16_rgb24(void)
{
    static u16 ys[W * H], us[(W / 2) * H], vs[(W / 2) * H];
    static u8 out[W * H * 3];
    for (int i = 0; i < W * H; i++)
        ys[i] = (u16)(0xAB00 | (u8)(5 * i));
    for (int i = 0; i < (W / 2) * H; i++) {
        us[i] = (u16)(0xCD00 | (u8)(128 + i));
        vs[i] = (u16)(0xEF00 | (u8)(128 - i));
    }
    setup(INPUT_YUV422_INDIV_16, OUTPUT_RGB_24, BLOCK_LINE, 0);
    Y2RU_SetSendingY(ys, sizeof ys, sizeof ys, 0);
    Y2RU_SetSendingU(us, sizeof us, sizeof us, 0);
    Y2RU_SetSendingV(vs, sizeof vs, sizeof vs, 0);
    Y2RU_SetReceiving(out, sizeof out, sizeof out, 0);
    if (!convert())
        return;
    Y2RU_ColorCoefficients c;
    Y2RU_GetStandardCoefficient(&c, COEFFICIENT_ITU_R_BT_601);
    int bad = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int i = y * W + x, ci = y * (W / 2) + x / 2;
            int Y = ys[i] & 0xFF, U = us[ci] & 0xFF, V = vs[ci] & 0xFF;
            const u8 *p = out + 3 * i;      /* B, G, R */
            if ((p[0] != expect(&c, Y, U, V, 2) || p[1] != expect(&c, Y, U, V, 1) || p[2] != expect(&c, Y, U, V, 0))
                && bad++ < 3)
                say("  pixel (%d, %d) wrong\n", x, y);
        }
    say("%s: 4:2:2 planes with 16-bit samples -> RGB888\n", bad ? "FAIL" : "PASS");
}

/* The presets behave like their standards. */
static void test_presets(void)
{
    static const char *names[4] = { "BT.601", "BT.709", "BT.601 TV range", "BT.709 TV range" };
    for (int s = 0; s < 4; s++) {
        Y2RU_ColorCoefficients c;
        Y2RU_GetStandardCoefficient(&c, (Y2RU_StandardCoefficient)s);
        int ok = 1;
        if (s < 2) {            /* full range: grey stays grey */
            for (int ch = 0; ch < 3; ch++)
                ok &= abs(expect(&c, 128, 128, 128, ch) - 128) <= 1 && expect(&c, 255, 128, 128, ch) >= 254
                      && expect(&c, 0, 128, 128, ch) <= 1;
        } else {                /* TV range: 16 is black, 235 is white */
            for (int ch = 0; ch < 3; ch++)
                ok &= expect(&c, 16, 128, 128, ch) <= 1 && expect(&c, 235, 128, 128, ch) >= 254;
        }
        say("%s: %s preset: grey, black and white come out right\n", ok ? "PASS" : "FAIL", names[s]);
    }
}

int main(void)
{
    consoleDebugInit(debugDevice_SVC);
    if (R_FAILED(y2rInit())) {
        say("FAIL: y2rInit\n");
        return 1;
    }
    Y2RU_GetTransferEndEvent(&end_event);
    test_420_rgba();
    test_yuyv_565_tiled();
    test_422_16_rgb24();
    test_presets();
    y2rExit();
    say("done\n");
    return 0;
}
