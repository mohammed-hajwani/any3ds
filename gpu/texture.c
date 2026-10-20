/* Textures: decoding the 14 texel formats and sampling (wrap, filtering,
 * mipmaps, cube maps). From 3dbrew's GPU/Internal_Registers (texturing).
 *
 * Gotchas:
 *  - Textures are tiled like render targets (8x8 tiles, Morton order
 *    inside), and t = 0 is the *last* memory row: the GPU's origin is the
 *    bottom-left corner, so render-to-texture keeps its orientation.
 *  - Multi-byte texels are little-endian words with the first channel in
 *    the top bits: RGBA8 is stored A, B, G, R.
 *  - 4-bit formats hold two texels per byte, the even one in the low
 *    nibble.
 *  - ETC1 data is 4x4-pixel blocks, four per 8x8 tile (in Z order), each a
 *    64-bit little-endian word laid out as standard ETC1 (which is usually
 *    stored big-endian). ETC1A4 puts a 64-bit word of 4-bit alphas first. */
#include <inttypes.h>
#include <math.h>
#include <string.h>

#include "pica.h"
#include "texture.h"

enum { TEX_RGBA8, TEX_RGB8, TEX_RGBA5551, TEX_RGB565, TEX_RGBA4, TEX_IA8, TEX_HILO8, TEX_I8, TEX_A8,
       TEX_IA44, TEX_I4, TEX_A4, TEX_ETC1, TEX_ETC1A4 };

static const uint32_t bits_per_texel[14] = { 32, 24, 16, 16, 16, 16, 16, 8, 8, 8, 4, 4, 4, 8 };

uint32_t tex_level_size(uint32_t format, uint32_t width, uint32_t height)
{
    return width * height * bits_per_texel[format] / 8;
}

static uint8_t from4(uint32_t v) { return (uint8_t)(v * 17); }

/* ---- ETC1 ---- */

static const int etc1_modifiers[8][2] = {
    { 2, 8 }, { 5, 17 }, { 9, 29 }, { 13, 42 }, { 18, 60 }, { 24, 80 }, { 33, 106 }, { 47, 183 },
};

static int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* Pixel (x, y) of a 4x4 ETC1 block (y = row within the block). */
Rgba etc1_pixel(uint64_t block, uint32_t x, uint32_t y)
{
    int diff = block >> 33 & 1, flip = block >> 32 & 1;
    int second = flip ? y >= 2 : x >= 2;
    int r, g, b;
    if (diff) {
        int r1 = block >> 59 & 31, g1 = block >> 51 & 31, b1 = block >> 43 & 31;
        int dr = (int)((block >> 56 & 7) << 29) >> 29, dg = (int)((block >> 48 & 7) << 29) >> 29,
            db = (int)((block >> 40 & 7) << 29) >> 29;
        if (second) {
            r1 += dr;
            g1 += dg;
            b1 += db;
        }
        r = r1 << 3 | r1 >> 2;
        g = g1 << 3 | g1 >> 2;
        b = b1 << 3 | b1 >> 2;
    } else {
        int shift = second ? 0 : 4;
        r = (int)(block >> (56 + shift) & 15) * 17;
        g = (int)(block >> (48 + shift) & 15) * 17;
        b = (int)(block >> (40 + shift) & 15) * 17;
    }
    int table = (int)(block >> (second ? 34 : 37) & 7);
    uint32_t i = x * 4 + y;         /* pixel indices run down the columns */
    int msb = block >> (16 + i) & 1, lsb = block >> i & 1;
    int mod = etc1_modifiers[table][lsb];
    if (msb)
        mod = -mod;
    return (Rgba){ (uint8_t)clamp8(r + mod), (uint8_t)clamp8(g + mod), (uint8_t)clamp8(b + mod), 255 };
}

/* ---- texel fetch ---- */

/* Texel at memory position (mx, my) (my = 0 is the first row in memory)
   of a level stored at p, `width` texels wide. */
Rgba tex_decode(const uint8_t *p, uint32_t format, uint32_t width, uint32_t mx, uint32_t my)
{
    if (format == TEX_ETC1 || format == TEX_ETC1A4) {
        uint32_t block_size = format == TEX_ETC1 ? 8 : 16;
        uint32_t tile = (my / 8) * (width / 8) + mx / 8, sub = (my % 8 / 4) * 2 + mx % 8 / 4;
        const uint8_t *b = p + (tile * 4 + sub) * block_size;
        uint64_t alpha = UINT64_MAX, color;
        if (format == TEX_ETC1A4) {
            memcpy(&alpha, b, 8);
            b += 8;
        }
        memcpy(&color, b, 8);
        Rgba c = etc1_pixel(color, mx % 4, my % 4);
        c.a = from4((uint32_t)(alpha >> (4 * ((mx % 4) * 4 + my % 4))) & 15);
        return c;
    }
    uint32_t i = tile_index(mx, my, width);
    const uint8_t *t = p + i * bits_per_texel[format] / 8;
    switch (format) {
    case TEX_RGBA8:    return color_decode(t, PIXEL_RGBA8);
    case TEX_RGB8:     return color_decode(t, PIXEL_RGB8);
    case TEX_RGBA5551: return color_decode(t, PIXEL_RGB5A1);
    case TEX_RGB565:   return color_decode(t, PIXEL_RGB565);
    case TEX_RGBA4:    return color_decode(t, PIXEL_RGBA4);
    case TEX_IA8:      return (Rgba){ t[1], t[1], t[1], t[0] };
    case TEX_HILO8:    return (Rgba){ t[1], t[0], 0, 255 };
    case TEX_I8:       return (Rgba){ t[0], t[0], t[0], 255 };
    case TEX_A8:       return (Rgba){ 0, 0, 0, t[0] };
    case TEX_IA44:     return (Rgba){ from4(t[0] >> 4), from4(t[0] >> 4), from4(t[0] >> 4), from4(t[0] & 15) };
    case TEX_I4: {
        uint8_t n = from4(i & 1 ? t[0] >> 4 : t[0] & 15);
        return (Rgba){ n, n, n, 255 };
    }
    case TEX_A4:       return (Rgba){ 0, 0, 0, from4(i & 1 ? t[0] >> 4 : t[0] & 15) };
    default:           gpu_fatal("texture format %" PRIu32, format);
    }
}

/* ---- units ---- */

static const uint32_t unit_regs[3][6] = {      /* border, dim, param, lod, addr, type */
    { 0x081, 0x082, 0x083, 0x084, 0x085, 0x08E },
    { 0x091, 0x092, 0x093, 0x094, 0x095, 0x096 },
    { 0x099, 0x09A, 0x09B, 0x09C, 0x09D, 0x09E },
};

void tex_unit_read(int unit, TexUnit *t)
{
    const uint32_t *r = unit_regs[unit];
    uint32_t dim = pica.reg[r[1]];
    t->height = dim & 0x7FF;
    t->width = dim >> 16 & 0x7FF;
    t->param = pica.reg[r[2]];
    t->lod = pica.reg[r[3]];
    t->format = pica.reg[r[5]] & 0xF;
    t->border = pica.reg[r[0]];
    t->type = unit == 0 ? (int)(t->param >> 28 & 7) : 0;
    if (t->format > TEX_ETC1A4)
        gpu_fatal("texture unit %d format %" PRIu32, unit, t->format);
    if (t->type != 0 && t->type != 1)
        gpu_fatal("texture unit %d type %d (shadow/projection textures are not implemented yet)", unit, t->type);
    if (t->width && t->height       /* (0: never set up, frag_texture reads zero) */
            && (t->width < 8 || t->height < 8 || t->width % 8 || t->height % 8))
        gpu_fatal("texture unit %d is %" PRIu32 "x%" PRIu32 ", not a multiple of 8", unit, t->width, t->height);
    uint32_t first = pica.reg[r[4]];
    t->addr[0] = first << 3;
    for (int f = 1; f < 6; f++)                 /* cube faces: low 22 bits, top 6 from the first */
        t->addr[f] = ((first & 0xFC00000u) | (pica.reg[0x086 + f - 1] & 0x3FFFFF)) << 3;
}

/* Wrap a texel coordinate into [0, size); for clamp-to-border, outside
   coordinates set *border instead. */
static int wrap(int c, int size, uint32_t mode, int *border)
{
    switch (mode) {
    case 0:  return c < 0 ? 0 : c >= size ? size - 1 : c;                  /* clamp to edge */
    case 1:                                                                /* clamp to border */
        if (c < 0 || c >= size)
            *border = 1;
        return c;
    case 2:  return ((c % size) + size) % size;                            /* repeat */
    case 3: {                                                              /* mirrored repeat */
        int period = 2 * size, m = ((c % period) + period) % period;
        return m < size ? m : period - 1 - m;
    }
    default: gpu_fatal("texture wrap mode %" PRIu32, mode);
    }
}

static Rgba border_color(const TexUnit *t)
{
    uint32_t b = t->border;
    return (Rgba){ (uint8_t)b, (uint8_t)(b >> 8), (uint8_t)(b >> 16), (uint8_t)(b >> 24) };
}

static Rgba fetch(const TexUnit *t, const uint8_t *level, uint32_t w, uint32_t h, int x, int y)
{
    int border = 0;
    int wx = wrap(x, (int)w, t->param >> 12 & 7, &border), wy = wrap(y, (int)h, t->param >> 8 & 7, &border);
    if (border)
        return border_color(t);
    return tex_decode(level, t->format, w, (uint32_t)wx, h - 1 - (uint32_t)wy);
}

static Rgba sample_level(const TexUnit *t, const uint8_t *level, uint32_t w, uint32_t h, float s, float tc, int linear)
{
    float x = s * (float)w, y = tc * (float)h;
    if (!linear)
        return fetch(t, level, w, h, (int)floorf(x), (int)floorf(y));
    x -= 0.5f;
    y -= 0.5f;
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    float fx = x - (float)x0, fy = y - (float)y0;
    Rgba c00 = fetch(t, level, w, h, x0, y0), c10 = fetch(t, level, w, h, x0 + 1, y0);
    Rgba c01 = fetch(t, level, w, h, x0, y0 + 1), c11 = fetch(t, level, w, h, x0 + 1, y0 + 1);
    uint8_t out[4];
    const uint8_t *p00 = &c00.r, *p10 = &c10.r, *p01 = &c01.r, *p11 = &c11.r;
    for (int k = 0; k < 4; k++) {
        float top = p00[k] + (p10[k] - p00[k]) * fx, bottom = p01[k] + (p11[k] - p01[k]) * fx;
        out[k] = (uint8_t)lroundf(top + (bottom - top) * fy);
    }
    return (Rgba){ out[0], out[1], out[2], out[3] };
}

/* Pick the cube face from the major axis of (u, v, w) (OpenGL's rules)
   and turn the direction into 2D coordinates on it. */
static int cube_face(float u, float v, float w, float *s, float *t)
{
    float au = fabsf(u), av = fabsf(v), aw = fabsf(w), sc, tc, ma;
    int face;
    if (au >= av && au >= aw) {
        face = u >= 0 ? 0 : 1;
        ma = au;
        sc = u >= 0 ? -w : w;
        tc = -v;
    } else if (av >= aw) {
        face = v >= 0 ? 2 : 3;
        ma = av;
        sc = u;
        tc = v >= 0 ? w : -w;
    } else {
        face = w >= 0 ? 4 : 5;
        ma = aw;
        sc = w >= 0 ? u : -u;
        tc = -v;
    }
    *s = (sc / ma + 1) * 0.5f;
    *t = (tc / ma + 1) * 0.5f;
    return face;
}

Rgba tex_sample(const TexUnit *t, float u, float v, float w, float lod)
{
    int face = 0;
    if (t->type == 1)
        face = cube_face(u, v, w, &u, &v);

    /* Level of detail: base + bias, clamped to [min, max]; minification
       when it's above 0. */
    float bias = (float)((int32_t)(t->lod << 19) >> 19) / 256.0f;     /* fixed1.4.8 */
    uint32_t max_level = t->lod >> 16 & 15, min_level = t->lod >> 24 & 15;
    lod += bias;
    int minify = lod > 0;
    int linear = (int)(minify ? t->param >> 2 & 1 : t->param >> 1 & 1);
    float clamped = fmaxf((float)min_level, fminf((float)max_level, lod));
    int mip_linear = t->param >> 24 & 1;
    uint32_t level = minify ? (uint32_t)(mip_linear ? floorf(clamped) : lroundf(clamped)) : min_level;
    if (level > max_level)
        level = max_level;

    uint32_t base = gpu_phys_to_virt(t->addr[face]);
    if (!base)
        gpu_fatal("texture at physical %08" PRIx32 " is unmapped", t->addr[face]);
    uint32_t offset = 0, lw = t->width, lh = t->height;
    for (uint32_t l = 0; l < level && lw > 8 && lh > 8; l++) {
        offset += tex_level_size(t->format, lw, lh);
        lw /= 2;
        lh /= 2;
    }
    const uint8_t *p = gpu_mem(base + offset, tex_level_size(t->format, lw, lh));
    Rgba c = sample_level(t, p, lw, lh, u, v, linear);
    if (minify && mip_linear && level < max_level && lw > 8 && lh > 8 && clamped > (float)level) {
        float f = clamped - (float)level;
        uint32_t next = base + offset + tex_level_size(t->format, lw, lh);
        const uint8_t *q = gpu_mem(next, tex_level_size(t->format, lw / 2, lh / 2));
        Rgba d = sample_level(t, q, lw / 2, lh / 2, u, v, linear);
        c = (Rgba){ (uint8_t)lroundf(c.r + (d.r - c.r) * f), (uint8_t)lroundf(c.g + (d.g - c.g) * f),
                    (uint8_t)lroundf(c.b + (d.b - c.b) * f), (uint8_t)lroundf(c.a + (d.a - c.a) * f) };
    }
    return c;
}

/* ---- texture unit 3: procedural textures ----
 * (3dbrew: GPU/Procedural_Texture_Generation and the PROCTEX registers.)
 * The coordinates (u, v) are wrapped per direction, turned into one value
 * g by a base-shape function (u, u^2, (u + v) / 2, sqrt(u^2 + v^2), ...),
 * mapped through a 128-entry table F, and F(g) picks a colour from the
 * colour table. With "separate alpha", alpha is its own F(g) directly.
 * The noise module and the coordinate shifts are not publicly documented,
 * so a texture that enables them stops by name rather than guessing. */

static float proctex_wrap(float c, uint32_t mode)
{
    switch (mode) {
    case 0:  return c > 1 ? 0 : fabsf(c);                       /* clamp to zero */
    case 1:  return fminf(fabsf(c), 1);                         /* clamp to edge */
    case 2:  return fabsf(c) - floorf(fabsf(c));                /* symmetrical repeat */
    case 3: {                                                   /* mirrored repeat */
        float m = fmodf(fabsf(c), 2);
        return m <= 1 ? m : 2 - m;
    }
    default: gpu_fatal("procedural texture clamp mode %" PRIu32 " is not implemented", mode);
    }
}

static float proctex_shape(uint32_t fn, float u, float v)
{
    switch (fn) {
    case 0:  return u;
    case 1:  return u * u;
    case 2:  return v;
    case 3:  return v * v;
    case 4:  return (u + v) / 2;
    case 5:  return (u * u + v * v) / 2;
    case 6:  return sqrtf(u * u + v * v);
    case 7:  return fminf(u, v);
    case 8:  return fmaxf(u, v);
    default: gpu_fatal("procedural texture base shape %" PRIu32 " is not implemented", fn);
    }
}

/* A 128-entry mapping table: 12-bit value + 12-bit signed difference to
   the next entry, interpolated. Returns 0..1. */
static float proctex_map(const uint32_t *table, float g)
{
    float x = fminf(fmaxf(g, 0), 1) * 128;
    int i = x >= 127 ? 127 : (int)x;
    float frac = x - (float)i;
    uint32_t e = table[i];
    int value = (int)(e & 0xFFF), diff = (int)((e >> 12 & 0xFFF) << 20) >> 20;
    float f = ((float)value + (float)diff * frac) / 4096;
    return fminf(fmaxf(f, 0), 1);
}

Rgba proctex_sample(float u, float v)
{
    uint32_t p0 = pica.reg[0x0A8], p4 = pica.reg[0x0AC], p5 = pica.reg[0x0AD];
    if (p0 >> 15 & 1)
        gpu_fatal("procedural texture noise is not implemented (not publicly documented)");
    if (p0 >> 16 & 0xF)
        gpu_fatal("procedural texture coordinate shift is not implemented (not publicly documented)");
    u = proctex_wrap(u, p0 & 7);
    v = proctex_wrap(v, p0 >> 3 & 7);
    float g_rgb = proctex_shape(p0 >> 6 & 0xF, u, v);
    float f_rgb = proctex_map(pica.proctex_lut[2], g_rgb);

    /* The colour table: `width` entries from the base-level offset;
       linear filtering adds the (halved) difference table. */
    uint32_t width = p4 >> 11 & 0xFF, offset = p5 & 0xFF, filter = p4 & 7;
    if (!width)
        width = 1;
    float x = f_rgb * (float)(width - 1);
    uint32_t i = (uint32_t)x;
    float frac = x - (float)i;
    uint32_t c = pica.proctex_lut[4][(offset + i) & 0xFF], d = pica.proctex_lut[5][(offset + i) & 0xFF];
    uint8_t out[4];
    for (int k = 0; k < 4; k++) {
        float base = (float)(c >> (8 * k) & 0xFF);
        float diff = (float)(int8_t)(d >> (8 * k) & 0xFF) * 2;
        float val = (filter & 1) ? base + diff * frac : base;
        out[k] = (uint8_t)fminf(fmaxf(lroundf(val), 0), 255);
    }
    Rgba col = { out[0], out[1], out[2], out[3] };
    if (p0 >> 14 & 1) {                 /* separate alpha: its own shape and mapping */
        float g_a = proctex_shape(p0 >> 10 & 0xF, u, v);
        col.a = (uint8_t)lroundf(proctex_map(pica.proctex_lut[3], g_a) * 255);
    }
    return col;
}
