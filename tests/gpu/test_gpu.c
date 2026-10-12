/* Unit tests for gpu/: the pieces that can be checked against exact
 * expected values without running any guest code. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "context.h"
#include "gpu.h"
#include "pica.h"

static int failures, checks;

#define CHECK(cond, ...)                                         \
    do {                                                         \
        checks++;                                                \
        if (!(cond)) {                                           \
            failures++;                                          \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);          \
            printf(__VA_ARGS__);                                 \
            printf("\n");                                        \
        }                                                        \
    } while (0)

static uint8_t *mem;

/* ---- float24: 1 sign, 7 exponent (bias 63), 16 mantissa bits ---- */
static void test_float24(void)
{
    CHECK(f32_to_f24(1.0f) == 0x3F0000, "1.0 -> %06x", f32_to_f24(1.0f));
    CHECK(f32_to_f24(-2.5f) == 0xC04000, "-2.5 -> %06x", f32_to_f24(-2.5f));
    CHECK(f32_to_f24(0.0f) == 0, "0 -> %06x", f32_to_f24(0.0f));
    CHECK(f32_to_f24(-0.0f) == 0x800000, "-0 -> %06x", f32_to_f24(-0.0f));
    CHECK(f32_to_f24(1e-30f) == 0, "underflow flushes to zero: %06x", f32_to_f24(1e-30f));
    CHECK(f32_to_f24(1e30f) == 0x7F0000, "overflow saturates to infinity: %06x", f32_to_f24(1e30f));
    CHECK(f24_to_f32(0x3F0000) == 1.0f, "0x3f0000 -> %g", f24_to_f32(0x3F0000));
    CHECK(f24_to_f32(0xC04000) == -2.5f, "0xc04000 -> %g", f24_to_f32(0xC04000));
    CHECK(f24_to_f32(0x000123) == 0.0f, "a denormal reads as zero: %g", f24_to_f32(0x000123));
    CHECK(isinf(f24_to_f32(0x7F0000)) && f24_to_f32(0x7F0000) > 0, "0x7f0000 is +inf");
    CHECK(isinf(f24_to_f32(0xFF0000)) && f24_to_f32(0xFF0000) < 0, "0xff0000 is -inf");
    CHECK(isnan(f24_to_f32(0x7F0001)), "0x7f0001 is a NaN");
    /* Truncation, not rounding: 1 + 2^-17 keeps only 16 mantissa bits. */
    CHECK(f24_to_f32(f32_to_f24(1.0f + 1.0f / 131072)) == 1.0f, "mantissa truncated");
    for (int i = -200; i <= 200; i++) {         /* exact for values with <= 17 significant bits */
        float f = (float)i * 0.125f;
        CHECK(f24_to_f32(f32_to_f24(f)) == f, "round trip %g", f);
    }
}

/* ---- tiling: 8x8 tiles in rows, Morton (Z) order inside a tile ---- */
static void test_tiling(void)
{
    static const struct { uint32_t x, y, want; } cases[] = {
        { 0, 0, 0 }, { 1, 0, 1 }, { 0, 1, 2 }, { 1, 1, 3 }, { 2, 0, 4 }, { 0, 2, 8 },
        { 3, 3, 15 }, { 4, 0, 16 }, { 0, 4, 32 }, { 7, 7, 63 },
        { 8, 0, 64 }, { 15, 7, 127 }, { 0, 8, 128 }, { 9, 9, 128 + 64 + 3 },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint32_t got = tile_index(cases[i].x, cases[i].y, 16);
        CHECK(got == cases[i].want, "tile_index(%u, %u, 16) = %u, want %u", cases[i].x, cases[i].y, got, cases[i].want);
    }
}

/* ---- pixel formats ---- */
static void test_colors(void)
{
    uint8_t p[4];
    Rgba c = { 0x12, 0x34, 0x56, 0x78 };
    color_encode(p, PIXEL_RGBA8, c);
    CHECK(p[0] == 0x78 && p[1] == 0x56 && p[2] == 0x34 && p[3] == 0x12, "RGBA8 is stored A, B, G, R");
    color_encode(p, PIXEL_RGB8, c);
    CHECK(p[0] == 0x56 && p[1] == 0x34 && p[2] == 0x12, "RGB8 is stored B, G, R");
    Rgba white = { 255, 255, 255, 255 }, d;
    color_encode(p, PIXEL_RGB565, white);
    CHECK(p[0] == 0xFF && p[1] == 0xFF, "RGB565 white");
    d = color_decode(p, PIXEL_RGB565);
    CHECK(d.r == 255 && d.g == 255 && d.b == 255 && d.a == 255, "RGB565 decodes white (alpha 255)");
    Rgba red = { 255, 0, 0, 255 };
    color_encode(p, PIXEL_RGB565, red);
    CHECK(p[0] == 0x00 && p[1] == 0xF8, "RGB565 red is 0xf800: %02x%02x", p[1], p[0]);
    color_encode(p, PIXEL_RGB5A1, (Rgba){ 0, 0, 255, 0 });
    CHECK(p[0] == 0x3E && p[1] == 0x00, "RGB5A1 blue, alpha 0 is 0x003e: %02x%02x", p[1], p[0]);
    color_encode(p, PIXEL_RGBA4, (Rgba){ 0x10, 0x20, 0x30, 0xF0 });
    CHECK(p[0] == 0x3F && p[1] == 0x12, "RGBA4 is 0x123f: %02x%02x", p[1], p[0]);
    d = color_decode(p, PIXEL_RGBA4);
    CHECK(d.r == 0x11 && d.g == 0x22 && d.b == 0x33 && d.a == 0xFF, "RGBA4 expands n -> n * 17");
}

/* ---- GX MemoryFill ---- */
static void test_memory_fill(void)
{
    const uint32_t a = 0x14000000, n = 64;
    memset(mem + a - 16, 0xEE, n + 32);
    gx_memory_fill(a, 0x00ABCDEF, a + 24, 0x0001 | 0x0100);      /* 24-bit */
    CHECK(mem[a] == 0xEF && mem[a + 1] == 0xCD && mem[a + 2] == 0xAB && mem[a + 21] == 0xEF && mem[a + 23] == 0xAB,
          "24-bit fill repeats the 3 low bytes");
    CHECK(mem[a + 24] == 0xEE && mem[a - 1] == 0xEE, "24-bit fill stays inside [start, end)");
    gx_memory_fill(a, 0x12345678, a + 32, 0x0001 | 0x0200);      /* 32-bit */
    CHECK(mem[a] == 0x78 && mem[a + 3] == 0x12 && mem[a + 31] == 0x12 && mem[a + 32] == 0xEE, "32-bit fill");
    gx_memory_fill(a, 0xFFFF1234, a + 8, 0x0001);                 /* 16-bit */
    CHECK(mem[a] == 0x34 && mem[a + 1] == 0x12 && mem[a + 6] == 0x34 && mem[a + 7] == 0x12, "16-bit fill");
}

/* ---- GX DisplayTransfer: tiled render target -> linear framebuffer ---- */
static Rgba pattern(uint32_t x, uint32_t y)
{
    return (Rgba){ (uint8_t)(x * 8), (uint8_t)(y * 16), (uint8_t)(x ^ y), 255 };
}

static void test_display_transfer(void)
{
    const uint32_t in = 0x14100000, out = 0x14200000, w = 16, h = 8;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            color_encode(mem + in + 4 * tile_index(x, y, w), PIXEL_RGBA8, pattern(x, y));

    /* tiled RGBA8 -> linear RGB8, same size */
    memset(mem + out, 0, w * h * 3);
    gx_display_transfer(in, out, h << 16 | w, h << 16 | w, PIXEL_RGBA8 << 8 | PIXEL_RGB8 << 12);
    int ok = 1;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            Rgba got = color_decode(mem + out + 3 * (y * w + x), PIXEL_RGB8), want = pattern(x, y);
            ok &= got.r == want.r && got.g == want.g && got.b == want.b;
        }
    CHECK(ok, "tiled RGBA8 -> linear RGB8 keeps every pixel in place");

    /* vertical flip */
    gx_display_transfer(in, out, h << 16 | w, h << 16 | w, 1 | PIXEL_RGBA8 << 8 | PIXEL_RGB8 << 12);
    Rgba got = color_decode(mem + out + 3 * (0 * w + 5), PIXEL_RGB8), want = pattern(5, h - 1);
    CHECK(got.r == want.r && got.g == want.g && got.b == want.b, "flip: output row 0 is input row h-1");

    /* 2x2 downscale (anti-aliasing): each output pixel averages a 2x2 block */
    gx_display_transfer(in, out, h << 16 | w, (h / 2) << 16 | (w / 2), PIXEL_RGBA8 << 8 | PIXEL_RGB8 << 12 | 2u << 24);
    got = color_decode(mem + out + 3 * (1 * (w / 2) + 3), PIXEL_RGB8);
    Rgba p0 = pattern(6, 2), p1 = pattern(7, 2), p2 = pattern(6, 3), p3 = pattern(7, 3);
    CHECK(got.r == (p0.r + p1.r + p2.r + p3.r) / 4 && got.g == (p0.g + p1.g + p2.g + p3.g) / 4,
          "2x2 downscale averages: got r=%u g=%u", got.r, got.g);

    /* linear -> tiled (the other direction), RGBA8 -> RGB565 */
    for (uint32_t i = 0; i < w * h; i++)
        color_encode(mem + in + 4 * i, PIXEL_RGBA8, pattern(i % w, i / w));
    gx_display_transfer(in, out, h << 16 | w, h << 16 | w, 2 | PIXEL_RGBA8 << 8 | PIXEL_RGB565 << 12);
    got = color_decode(mem + out + 2 * tile_index(9, 3, w), PIXEL_RGB565);
    want = pattern(9, 3);
    CHECK(got.r >> 3 == want.r >> 3 && got.g >> 2 == want.g >> 2 && got.b >> 3 == want.b >> 3,
          "linear -> tiled RGB565");
}

/* ---- GX TextureCopy: a raw copy in lines, with gaps on each side ---- */
static void test_texture_copy(void)
{
    const uint32_t in = 0x14300000, out = 0x14400000;
    for (uint32_t i = 0; i < 256; i++)
        mem[in + i] = (uint8_t)i;
    memset(mem + out, 0xEE, 256);
    /* input: lines of 32 bytes (2 units) with a 16-byte (1 unit) gap; output: lines of 16 bytes, no gap */
    gx_texture_copy(in, out, 64, 1u << 16 | 2, 0u << 16 | 1, 8);
    CHECK(mem[out] == 0 && mem[out + 31] == 31, "first input line copied");
    CHECK(mem[out + 32] == 48 && mem[out + 63] == 79, "input gap skipped: %u %u", mem[out + 32], mem[out + 63]);
    CHECK(mem[out + 64] == 0xEE, "copies exactly `size` bytes");
}

/* ---- command lists ---- */
static uint32_t cl_words[256], cl_n;

static void cl_cmd(uint32_t id, uint32_t mask, int consecutive, const uint32_t *params, uint32_t count)
{
    cl_words[cl_n++] = params[0];
    cl_words[cl_n++] = id | mask << 16 | (count - 1) << 20 | (uint32_t)consecutive << 31;
    for (uint32_t i = 1; i < count; i++)
        cl_words[cl_n++] = params[i];
    if (cl_n & 1)
        cl_words[cl_n++] = 0;       /* pad to 8 bytes */
}

static void cl_run(void)
{
    const uint32_t at = 0x14500000;
    memcpy(mem + at, cl_words, cl_n * 4);
    pica_run_command_list(at, cl_n * 4);
    cl_n = 0;
}

/* libctru's float24 vec4 packing: x, y, z, w as 24-bit fields (x lowest),
   sent as three words, highest first. */
static void pack_f24x4(float x, float y, float z, float w, uint32_t out[3])
{
    uint8_t b[12];
    uint32_t v[4] = { f32_to_f24(x), f32_to_f24(y), f32_to_f24(z), f32_to_f24(w) };
    for (int i = 0; i < 4; i++)
        memcpy(b + 3 * i, &v[i], 3);
    uint32_t packed[3];
    memcpy(packed, b, 12);
    out[0] = packed[2];
    out[1] = packed[1];
    out[2] = packed[0];
}

static void test_command_lists(void)
{
    /* A plain write, then a masked write that only touches byte 1. */
    cl_cmd(0x041, 0xF, 0, (uint32_t[]){ 0x11223344 }, 1);
    cl_cmd(0x041, 0x2, 0, (uint32_t[]){ 0xAABBCCDD }, 1);
    /* Consecutive mode: three registers in one command. */
    cl_cmd(0x0C0, 0xF, 1, (uint32_t[]){ 1, 2, 3 }, 3);
    /* Non-consecutive: shader code words all go to the code FIFO. */
    cl_cmd(REG_VSH_BASE + SH_CODETRANSFER_INDEX, 0xF, 0, (uint32_t[]){ 10 }, 1);
    cl_cmd(REG_VSH_BASE + SH_CODETRANSFER_DATA, 0xF, 0, (uint32_t[]){ 0xA, 0xB, 0xC }, 3);
    /* Float uniforms: c5 and c6 in float32 mode (each sent w, z, y, x; the
       index advances by itself), then c7 in float24 mode. */
    cl_cmd(REG_VSH_BASE + SH_FLOATUNIFORM_INDEX, 0xF, 0, (uint32_t[]){ 0x80000005 }, 1);
    float f[8] = { 4.0f, 3.0f, 2.0f, 1.0f, 8.0f, 7.0f, 6.0f, 5.0f };
    uint32_t fw[8];
    memcpy(fw, f, sizeof fw);
    cl_cmd(REG_VSH_BASE + SH_FLOATUNIFORM_DATA, 0xF, 0, fw, 8);
    uint32_t f24[3];
    cl_cmd(REG_VSH_BASE + SH_FLOATUNIFORM_INDEX, 0xF, 0, (uint32_t[]){ 7 }, 1);
    pack_f24x4(-1.5f, 0.25f, 8.0f, 100.0f, f24);
    cl_cmd(REG_VSH_BASE + SH_FLOATUNIFORM_DATA, 0xF, 0, f24, 3);
    /* A fixed attribute. */
    cl_cmd(REG_FIXEDATTRIB_INDEX, 0xF, 0, (uint32_t[]){ 3 }, 1);
    pack_f24x4(0.5f, 0.75f, -2.0f, 1.0f, f24);
    cl_cmd(REG_FIXEDATTRIB_DATA0, 0xF, 1, f24, 3);
    /* FINALIZE ends the list: the write after it must not happen. */
    cl_cmd(REG_FINALIZE, 0xF, 0, (uint32_t[]){ 0x12345678 }, 1);
    cl_cmd(0x042, 0xF, 0, (uint32_t[]){ 0xDEAD }, 1);
    cl_run();

    CHECK(pica.reg[0x041] == 0x1122CC44, "masked write keeps the other bytes: %08x", pica.reg[0x041]);
    CHECK(pica.reg[0x0C0] == 1 && pica.reg[0x0C1] == 2 && pica.reg[0x0C2] == 3, "consecutive writes");
    CHECK(pica.vsh.code[10] == 0xA && pica.vsh.code[11] == 0xB && pica.vsh.code[12] == 0xC, "shader code FIFO");
    Vec4 c5 = pica.vsh.uniform[5], c6 = pica.vsh.uniform[6], c7 = pica.vsh.uniform[7], a3 = pica.fixed_attrib[3];
    CHECK(c5.x == 1.0f && c5.y == 2.0f && c5.z == 3.0f && c5.w == 4.0f, "float32 uniform: %g %g %g %g",
          c5.x, c5.y, c5.z, c5.w);
    CHECK(c6.x == 5.0f && c6.w == 8.0f, "float32 uniform, index advanced: %g %g %g %g", c6.x, c6.y, c6.z, c6.w);
    CHECK(c7.x == -1.5f && c7.y == 0.25f && c7.z == 8.0f && c7.w == 100.0f, "float24 uniform: %g %g %g %g",
          c7.x, c7.y, c7.z, c7.w);
    CHECK(a3.x == 0.5f && a3.y == 0.75f && a3.z == -2.0f && a3.w == 1.0f, "fixed attribute 3: %g %g %g %g",
          a3.x, a3.y, a3.z, a3.w);
    CHECK(pica.reg[0x042] != 0xDEAD, "nothing runs after FINALIZE");
}

int main(void)
{
    mem = guest_mem_alloc();
    gpu_init(mem);
    test_float24();
    test_tiling();
    test_colors();
    test_memory_fill();
    test_display_transfer();
    test_texture_copy();
    test_command_lists();
    guest_mem_free(mem);
    printf("%s: %d checks, %d failed\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures != 0;
}
