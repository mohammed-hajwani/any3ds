/* Unit tests for gpu/: the pieces that can be checked against exact
 * expected values without running any guest code. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "context.h"
#include "gpu.h"
#include "pica.h"
#include "texture.h"

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

/* ---- the shader interpreter, on a program assembled by picasso ---- */

/* Load a .shbin (DVLB): program code + operand descriptors from the DVLP,
   the first DVLE's entry point and constants (as libctru's shbin.c reads
   them). */
static int load_shbin(const char *path, ShaderUnit *u, uint32_t *entry, uint16_t *bools, uint8_t ints[4][4])
{
    static uint32_t file[16384];
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    size_t n = fread(file, 1, sizeof file, f);
    fclose(f);
    if (n < 16 || memcmp(file, "DVLB", 4) != 0)
        return 0;
    uint32_t ndvle = file[1];
    const uint32_t *dvlp = &file[2 + ndvle];
    memset(u, 0, sizeof *u);
    for (uint32_t i = 0; i < dvlp[3] && i < 4096; i++)
        u->code[i] = dvlp[dvlp[2] / 4 + i];
    for (uint32_t i = 0; i < dvlp[5] && i < 128; i++)
        u->opdesc[i] = dvlp[dvlp[4] / 4 + 2 * i];
    const uint32_t *dvle = &file[file[2] / 4];
    *entry = dvle[2];
    const uint32_t *consts = &dvle[dvle[6] / 4];
    for (uint32_t i = 0; i < dvle[7]; i++) {
        const uint32_t *c = &consts[5 * i];         /* u16 type, u16 id, u32 data[4] */
        uint32_t type = c[0] & 0xFFFF, id = c[0] >> 16;
        if (type == 0 && (c[1] & 1))
            *bools |= (uint16_t)(1u << id);
        else if (type == 1 && id < 4)
            memcpy(ints[id], &c[1], 4);
        else if (type == 2 && (id & 0xFF) < 96)
            u->uniform[id & 0xFF] = (Vec4){ f24_to_f32(c[1]), f24_to_f32(c[2]), f24_to_f32(c[3]), f24_to_f32(c[4]) };
    }
    return 1;
}

static int vec_is(Vec4 v, float x, float y, float z, float w)
{
    return v.x == x && v.y == y && v.z == z && v.w == w;
}

#define CHECK_VEC(v, X, Y, Z, W, what)     CHECK(vec_is(v, X, Y, Z, W), "%s: got (%g, %g, %g, %g), want (%g, %g, %g, %g)", what,           (double)(v).x, (double)(v).y, (double)(v).z, (double)(v).w, (double)(X), (double)(Y), (double)(Z), (double)(W))

static void test_shader(const char *shbin)
{
    static ShaderUnit unit;
    uint32_t entry = 0;
    uint16_t bools = 0;
    uint8_t ints[4][4] = { { 0 } };
    if (!load_shbin(shbin, &unit, &entry, &bools, ints)) {
        CHECK(0, "can't load %s", shbin);
        return;
    }
    ShaderRegs s;
    memset(&s, 0, sizeof s);
    s.v[0] = (Vec4){ 1, 2, 3, 4 };
    s.v[1] = (Vec4){ 0.5f, -1, 2, 8 };
    s.v[2] = (Vec4){ 10, 20, 30, 40 };
    s.v[3] = (Vec4){ 2, 1, 0, 0 };
    shader_exec(&unit, entry, bools, ints, &s);
    CHECK_VEC(s.o[0], 1.5f, 1, 5, 12, "add");
    CHECK_VEC(s.o[1], 2, 1, -3, 1, "mul by a constant");
    CHECK_VEC(s.o[2], 4.5f, 36.5f, 12.5f, 2, "dp3, dp4, dph, rcp");
    CHECK_VEC(s.o[3], 10.5f, 18, 36, 72, "mad");
    CHECK_VEC(s.o[4], -4, -3, -2, -1, "negated swizzle -v0.wzyx");
    CHECK_VEC(s.o[5], 1, 2, 2, 4, "max/min under write masks");
    CHECK_VEC(s.o[6], 120, 121, 122, 123, "relative addressing c[a0.x]");
    CHECK_VEC(s.o[7], 2, -1, 3, 7, "cmp + ifc/else, loop count, call + ifu");
    CHECK_VEC(s.o[8], 2, -3, 0.25f, 1, "flr, rsq, sge");
    CHECK_VEC(s.o[9], 8, 4, 0, 0, "ex2, lg2, slt, no negative zero");
    CHECK(!signbit(s.o[9].w), "the zero is +0");
    CHECK_VEC(s.o[10], 3, 1, 0, 0, "breakc leaves the loop");
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

/* ---- the rasteriser, through a real command list ---- */

static uint32_t f24(float f) { return f32_to_f24(f); }

/* Configure a 16x16 RGBA8 render target, the passthrough shader (v0 =
   position, v1 = colour), immediate-mode triangles, additive blending. */
static void raster_setup(const ShaderUnit *pass, uint32_t entry, int with_depth)
{
    pica.vsh = *pass;
    const uint32_t vsh = REG_VSH_BASE;
    cl_cmd(vsh + SH_INPUTBUFFER_CONFIG, 0xF, 0, (uint32_t[]){ 0xA0000001 }, 1);
    cl_cmd(vsh + SH_ENTRYPOINT, 0xF, 0, (uint32_t[]){ 0x7FFF0000 | entry }, 1);
    cl_cmd(vsh + SH_ATTRIBUTES_PERMUTATION_LOW, 0xF, 0, (uint32_t[]){ 0x10 }, 1);
    cl_cmd(vsh + SH_OUTMAP_MASK, 0xF, 0, (uint32_t[]){ 0x3 }, 1);
    cl_cmd(REG_SH_OUTMAP_TOTAL, 0xF, 0, (uint32_t[]){ 2 }, 1);
    cl_cmd(REG_SH_OUTMAP_O0, 0xF, 1, (uint32_t[]){ 0x03020100, 0x0B0A0908 }, 2);
    cl_cmd(REG_VSH_NUM_ATTR, 0xF, 0, (uint32_t[]){ 1 }, 1);
    cl_cmd(REG_PRIMITIVE_CONFIG, 0xF, 0, (uint32_t[]){ 0x0001 }, 1);           /* triangles */
    cl_cmd(REG_VIEWPORT_WIDTH, 0xF, 0, (uint32_t[]){ f24(8) }, 1);               /* width / 2 */
    cl_cmd(REG_VIEWPORT_HEIGHT, 0xF, 0, (uint32_t[]){ f24(8) }, 1);
    cl_cmd(REG_VIEWPORT_XY, 0xF, 0, (uint32_t[]){ 0 }, 1);
    cl_cmd(REG_DEPTHMAP_ENABLE, 0xF, 0, (uint32_t[]){ 1 }, 1);
    cl_cmd(REG_DEPTHMAP_SCALE, 0xF, 0, (uint32_t[]){ f24(-1) }, 1);             /* depth = -z/w */
    cl_cmd(REG_DEPTHMAP_OFFSET, 0xF, 0, (uint32_t[]){ 0 }, 1);
    cl_cmd(REG_COLORBUFFER_LOC, 0xF, 0, (uint32_t[]){ 0x20600000 >> 3 }, 1);     /* virtual 0x14600000 */
    cl_cmd(REG_DEPTHBUFFER_LOC, 0xF, 0, (uint32_t[]){ 0x20700000 >> 3 }, 1);
    cl_cmd(REG_COLORBUFFER_FORMAT, 0xF, 0, (uint32_t[]){ 2 }, 1);               /* RGBA8 */
    cl_cmd(REG_DEPTHBUFFER_FORMAT, 0xF, 0, (uint32_t[]){ 2 }, 1);               /* 24-bit */
    cl_cmd(REG_FRAMEBUFFER_DIM, 0xF, 0, (uint32_t[]){ 16 | 15 << 12 | 1u << 24 }, 1);
    cl_cmd(REG_COLORBUFFER_READ, 0xF, 1, (uint32_t[]){ 0xF, 0xF, with_depth ? 2u : 0u, with_depth ? 2u : 0u }, 4);
    cl_cmd(0x0C0, 0xF, 1, (uint32_t[]){ 0, 0, 0, 0, 0 }, 5);                     /* stage 0: primary colour */
    for (uint32_t base = 0x0C8; base <= 0x0F8; base += base == 0x0D8 ? 0x18 : 8)
        cl_cmd(base, 0xF, 1, (uint32_t[]){ 0x000F000F, 0, 0, 0, 0 }, 5);         /* the rest: previous */
    cl_cmd(REG_COLOR_OPERATION, 0xF, 0, (uint32_t[]){ 0x00E40100 }, 1);          /* blend */
    cl_cmd(REG_BLEND_FUNC, 0xF, 0, (uint32_t[]){ 1u << 16 | 1u << 20 | 1u << 24 }, 1);   /* src + dst */
    cl_cmd(REG_DEPTH_COLOR_MASK, 0xF, 0, (uint32_t[]){ (with_depth ? 0x1041u | 4u << 4 : 0u) | 0xF00 }, 1);
    cl_cmd(REG_FIXEDATTRIB_INDEX, 0xF, 0, (uint32_t[]){ 0xF }, 1);               /* immediate mode */
}

static void raster_vertex(float x, float y, float z, float r, float g, float b, float a)
{
    uint32_t w[3];
    pack_f24x4(x, y, z, 1.0f, w);
    cl_cmd(REG_FIXEDATTRIB_DATA0, 0xF, 1, w, 3);
    pack_f24x4(r, g, b, a, w);
    cl_cmd(REG_FIXEDATTRIB_DATA0, 0xF, 1, w, 3);
}

static Rgba fb_pixel(uint32_t x, uint32_t y)        /* y from the bottom, as the GPU counts */
{
    return color_decode(mem + 0x14600000 + 4 * tile_index(x, 15 - y, 16), PIXEL_RGBA8);
}

static void test_raster(const char *shbin)
{
    static ShaderUnit pass;
    uint32_t entry = 0;
    uint16_t bools = 0;
    uint8_t ints[4][4];
    if (!load_shbin(shbin, &pass, &entry, &bools, ints)) {
        CHECK(0, "can't load %s", shbin);
        return;
    }

    /* Two triangles sharing the square's diagonal, which runs through pixel
       centres: with additive blending, a pixel drawn twice would show. */
    memset(mem + 0x14600000, 0, 16 * 16 * 4);
    raster_setup(&pass, entry, 0);
    float q = 64.0f / 255;
    raster_vertex(-0.5f, -0.5f, -0.5f, q, 0, 0, 1);
    raster_vertex(0.5f, -0.5f, -0.5f, q, 0, 0, 1);
    raster_vertex(0.5f, 0.5f, -0.5f, q, 0, 0, 1);
    raster_vertex(-0.5f, -0.5f, -0.5f, q, 0, 0, 1);
    raster_vertex(0.5f, 0.5f, -0.5f, q, 0, 0, 1);
    raster_vertex(-0.5f, 0.5f, -0.5f, q, 0, 0, 1);
    cl_run();
    int once = 0, twice = 0, outside = 0;
    for (uint32_t y = 0; y < 16; y++)
        for (uint32_t x = 0; x < 16; x++) {
            int r = fb_pixel(x, y).r, inside = x >= 4 && x < 12 && y >= 4 && y < 12;
            once += inside && r == 64;
            twice += r > 64;
            outside += !inside && r != 0;
        }
    CHECK(once == 64 && twice == 0 && outside == 0,
          "square = 8x8 pixels, each drawn once: %d once, %d twice, %d outside", once, twice, outside);

    /* Depth: one triangle with z varying across it; the 24-bit depth at a
       pixel is the screen-space interpolation of -z. */
    memset(mem + 0x14700000, 0xFF, 16 * 16 * 4);
    raster_setup(&pass, entry, 1);
    raster_vertex(-1, -1, -0.2f, 1, 1, 1, 1);
    raster_vertex(1, -1, -0.6f, 1, 1, 1, 1);
    raster_vertex(-1, 1, -0.2f, 1, 1, 1, 1);
    cl_run();
    uint32_t d = 0;
    memcpy(&d, mem + 0x14700000 + 3 * tile_index(4, 15 - 3, 16), 3);   /* pixel (4, 3): centre x = 4.5 */
    float want = 0.2f + 0.4f * (4.5f / 16);                                 /* z changes along x only */
    CHECK(fabsf((float)d / 0xFFFFFF - want) < 1e-3f, "depth at (4, 3) = %f, want %f", (double)d / 0xFFFFFF, (double)want);
}

/* ---- textures ---- */

static int rgba_is(Rgba c, int r, int g, int b, int a)
{
    return c.r == r && c.g == g && c.b == b && c.a == a;
}

#define CHECK_RGBA(c, R, G, B, A, what)     CHECK(rgba_is(c, R, G, B, A), "%s: got (%d, %d, %d, %d), want (%d, %d, %d, %d)", what,           (c).r, (c).g, (c).b, (c).a, R, G, B, A)

static void test_texture_formats(void)
{
    uint8_t buf[64 * 4];
    /* Texel (1, 0) is index 1 in Morton order; (0, 1) is index 2. */
    memset(buf, 0, sizeof buf);
    buf[4 * 1 + 0] = 0x44; buf[4 * 1 + 1] = 0x33; buf[4 * 1 + 2] = 0x22; buf[4 * 1 + 3] = 0x11;
    CHECK_RGBA(tex_decode(buf, 0, 8, 1, 0), 0x11, 0x22, 0x33, 0x44, "RGBA8 stored A, B, G, R");
    memset(buf, 0, sizeof buf);
    buf[2 * 2 + 0] = 0x80; buf[2 * 2 + 1] = 0xC0;                         /* IA8: A, then I */
    CHECK_RGBA(tex_decode(buf, 5, 8, 0, 1), 0xC0, 0xC0, 0xC0, 0x80, "IA8");
    CHECK_RGBA(tex_decode(buf, 6, 8, 0, 1), 0xC0, 0x80, 0, 255, "HILO8: red = high byte");
    memset(buf, 0, sizeof buf);
    buf[2] = 0x7F;
    CHECK_RGBA(tex_decode(buf, 7, 8, 0, 1), 0x7F, 0x7F, 0x7F, 255, "I8");
    CHECK_RGBA(tex_decode(buf, 8, 8, 0, 1), 0, 0, 0, 0x7F, "A8");
    buf[1] = 0xA5;                                                        /* IA44: I high, A low */
    CHECK_RGBA(tex_decode(buf, 9, 8, 1, 0), 0xAA, 0xAA, 0xAA, 0x55, "IA44");
    memset(buf, 0, sizeof buf);
    buf[0] = 0x3C;                                                        /* texels 0 (low) and 1 (high) */
    CHECK_RGBA(tex_decode(buf, 10, 8, 0, 0), 0xCC, 0xCC, 0xCC, 255, "I4, even texel = low nibble");
    CHECK_RGBA(tex_decode(buf, 11, 8, 1, 0), 0, 0, 0, 0x33, "A4, odd texel = high nibble");
}

static void test_etc1(void)
{
    /* Individual mode, side by side: colours (A,5,3) and (1,2,3) as 4-bit,
       tables 1 and 0. */
    uint64_t b = 0;
    b |= (uint64_t)0xA << 60 | (uint64_t)0x1 << 56 | (uint64_t)0x5 << 52 | (uint64_t)0x2 << 48;
    b |= (uint64_t)0x3 << 44 | (uint64_t)0x3 << 40 | (uint64_t)1 << 37 | (uint64_t)0 << 34;
    b |= (uint64_t)1 << 29 | (uint64_t)1 << 13;                         /* pixel (3, 1): index 13 = -large */
    CHECK_RGBA(etc1_pixel(b, 0, 0), 0xAA + 5, 0x55 + 5, 0x33 + 5, 255, "ETC1 individual, +small modifier");
    CHECK_RGBA(etc1_pixel(b, 3, 1), 0x11 - 8, 0x22 - 8, 0x33 - 8, 255, "ETC1 second subblock, -large modifier");
    /* Differential mode, stacked (flip): R1 = 16, dR = -1, B1 = 31; table 7
       for the bottom half. */
    b = (uint64_t)16 << 59 | (uint64_t)7 << 56 | (uint64_t)31 << 43 | (uint64_t)7 << 34 | (uint64_t)1 << 33 | 1ull << 32;
    CHECK_RGBA(etc1_pixel(b, 1, 3), 15 * 8 + 3 + 47, 47, 255, 255, "ETC1 differential + flip");
    CHECK_RGBA(etc1_pixel(b, 1, 0), 16 * 8 + 4 + 2, 2, 255, 255, "ETC1 differential, first subblock");

    /* An 8x8 ETC1A4 texture: four 16-byte blocks in Z order. Block 3 (the
       bottom-right 4x4 in memory) is solid (17, 34, 51) + 2 with alpha 0x9. */
    uint8_t tex[64];
    memset(tex, 0, sizeof tex);
    uint64_t alpha = 0x9999999999999999ull, solid = (uint64_t)1 << 60 | (uint64_t)2 << 52 | (uint64_t)3 << 44;
    memcpy(tex + 48, &alpha, 8);
    memcpy(tex + 56, &solid, 8);
    CHECK_RGBA(tex_decode(tex, 13, 8, 5, 6), 17 + 2, 34 + 2, 51 + 2, 0x99, "ETC1A4 block 3 at (5, 6)");
}

static void test_texture_sampling(void)
{
    /* An 8x8 RGBA8 texture where texel (x, y) (y = 0 at the bottom) has
       red = 32x, green = 32y. */
    const uint32_t at = 0x14800000;
    for (uint32_t y = 0; y < 8; y++)
        for (uint32_t x = 0; x < 8; x++)
            color_encode(mem + at + 4 * tile_index(x, 7 - y, 8), PIXEL_RGBA8, (Rgba){ (uint8_t)(32 * x), (uint8_t)(32 * y), 0, 255 });
    TexUnit t = { .addr = { 0x20800000 }, .width = 8, .height = 8, .format = 0, .border = 0xFF0000FF };
    float c3 = 3.5f / 8, c6 = 6.5f / 8;
    t.param = 2u << 12 | 2u << 8;                                           /* repeat, nearest */
    CHECK_RGBA(tex_sample(&t, c3, c6, 0, -10), 96, 192, 0, 255, "nearest texel (3, 6); t = 0 is the bottom");
    CHECK_RGBA(tex_sample(&t, 1 + c3, c6 - 2, 0, -10), 96, 192, 0, 255, "repeat");
    t.param = 3u << 12 | 3u << 8;                                           /* mirrored repeat */
    CHECK_RGBA(tex_sample(&t, -c3, c6, 0, -10), 96, 192, 0, 255, "mirrored repeat");
    t.param = 0;                                                            /* clamp to edge */
    CHECK_RGBA(tex_sample(&t, 5, c6, 0, -10), 224, 192, 0, 255, "clamp to edge");
    t.param = 1u << 12 | 1u << 8;                                           /* clamp to border */
    CHECK_RGBA(tex_sample(&t, 5, c6, 0, -10), 255, 0, 0, 255, "clamp to border uses the border colour");
    CHECK_RGBA(tex_sample(&t, c3, c6, 0, -10), 96, 192, 0, 255, "clamp to border: inside, the texel itself");
    t.param = 1u << 1;                                                      /* linear magnification */
    CHECK_RGBA(tex_sample(&t, 4.0f / 8, c6, 0, -10), 112, 192, 0, 255, "linear: halfway between x = 3 and 4");

    /* Mipmaps: 16x16 level 0 all red, 8x8 level 1 all green right after it. */
    const uint32_t mip = 0x14810000;
    for (uint32_t i = 0; i < 256; i++)
        color_encode(mem + mip + 4 * i, PIXEL_RGBA8, (Rgba){ 255, 0, 0, 255 });
    for (uint32_t i = 0; i < 64; i++)
        color_encode(mem + mip + 1024 + 4 * i, PIXEL_RGBA8, (Rgba){ 0, 255, 0, 255 });
    TexUnit m = { .addr = { 0x20810000 }, .width = 16, .height = 16, .format = 0, .lod = 1u << 16 };
    CHECK_RGBA(tex_sample(&m, 0.5f, 0.5f, 0, 0.3f), 255, 0, 0, 255, "lod 0.3 picks level 0");
    CHECK_RGBA(tex_sample(&m, 0.5f, 0.5f, 0, 0.8f), 0, 255, 0, 255, "lod 0.8 picks level 1");
    CHECK_RGBA(tex_sample(&m, 0.5f, 0.5f, 0, 5.0f), 0, 255, 0, 255, "lod clamped to the max level");

    /* Cube map: six 8x8 faces, each one solid colour (face i: red = 40i). */
    TexUnit cube = { .width = 8, .height = 8, .format = 0, .type = 1 };
    for (uint32_t f = 0; f < 6; f++) {
        uint32_t face = 0x14820000 + 256 * f;
        for (uint32_t i = 0; i < 64; i++)
            color_encode(mem + face + 4 * i, PIXEL_RGBA8, (Rgba){ (uint8_t)(40 * f), 0, 0, 255 });
        cube.addr[f] = face - 0x14000000 + 0x20000000;
    }
    CHECK(tex_sample(&cube, 1, 0.1f, 0.2f, -10).r == 0, "+X face");
    CHECK(tex_sample(&cube, -1, 0.1f, 0.2f, -10).r == 40, "-X face");
    CHECK(tex_sample(&cube, 0.1f, -1, 0.2f, -10).r == 120, "-Y face");
    CHECK(tex_sample(&cube, 0.1f, 0.2f, 1, -10).r == 160, "+Z face");
}

int main(int argc, char **argv)
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
    test_texture_formats();
    test_etc1();
    test_texture_sampling();
    test_shader(argc > 1 ? argv[1] : "out/gpu/shader_test.shbin");
    test_raster(argc > 2 ? argv[2] : "out/gpu/passthrough.shbin");
    guest_mem_free(mem);
    printf("%s: %d checks, %d failed\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures != 0;
}
