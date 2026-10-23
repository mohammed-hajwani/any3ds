/* Drawing: vertex fetch -> vertex shader -> primitive assembly -> clipping
 * -> rasterisation -> colour combiners -> per-fragment tests -> blending
 * -> framebuffer. All in software, deterministic.
 *
 * Gotchas worth knowing:
 *  - Clip space: x and y in [-w, w], but z in [-w, 0] (not [-w, w] as in
 *    OpenGL). The depth written is z/w * DEPTHMAP_SCALE + DEPTHMAP_OFFSET.
 *  - Render targets are tiled 8x8 (see tile_index) and stored with the
 *    window's y = 0 in the last row: the GPU's origin is bottom-left.
 *  - The 3DS screens are rotated, so a 400x240 screen is a 240x400 render
 *    target; apps fold the rotation into their projection matrix.
 *
 * Rasterising is spread over threads (raster_flush), with the same result
 * as one thread: each pixel is drawn by one thread, in triangle order. */
#define _POSIX_C_SOURCE 200809L     /* sysconf */

#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pica.h"
#include "lighting.h"
#include "texture.h"

/* Vertex attributes by output semantic (SH_OUTMAP_Oi): position x/y/z/w,
   normal quaternion, colour r/g/b/a, texcoord0 u/v, texcoord1 u/v,
   texcoord0 w, view x/y/z, texcoord2 u/v. */
enum { SEM_POS = 0x00, SEM_QUAT = 0x04, SEM_COLOR = 0x08, SEM_TC0 = 0x0C, SEM_TC1 = 0x0E,
       SEM_TC0W = 0x10, SEM_VIEW = 0x12, SEM_TC2 = 0x16, NSEM = 0x18 };

typedef struct { float a[NSEM]; } Vertex;

/* Window-space vertex: x, y in pixels, z the depth value, and the other
   attributes divided by w (so they interpolate perspective-correctly)
   along with 1/w. */
typedef struct { float x, y, z, inv_w; float a[NSEM]; } WinVertex;

/* ---------------------------------------------------------------- vertices */

static float fetch(const uint8_t *p, uint32_t type)
{
    switch (type) {
    case 0:  return (float)(int8_t)p[0];
    case 1:  return (float)p[0];
    case 2:  { int16_t v; memcpy(&v, p, 2); return (float)v; }
    default: { float v; memcpy(&v, p, 4); return v; }
    }
}

static uint32_t attrib_format(uint32_t k)       /* 4 bits: type (0-1), element count - 1 (2-3) */
{
    uint64_t f = (uint64_t)pica.reg[REG_ATTRIBBUFFERS_FORMAT_HIGH] << 32 | pica.reg[REG_ATTRIBBUFFERS_FORMAT_LOW];
    return (uint32_t)(f >> (4 * k)) & 0xF;
}

/* The values of vertex `index`'s attributes 0..count-1, from the vertex
   arrays (or the fixed attributes). */
static void load_attributes(uint32_t index, Vec4 attr[12])
{
    uint32_t high = pica.reg[REG_ATTRIBBUFFERS_FORMAT_HIGH], fixed = high >> 16 & 0xFFF;
    uint32_t base = pica.reg[REG_ATTRIBBUFFERS_LOC] << 3;
    for (int k = 0; k < 12; k++)
        attr[k] = (fixed >> k & 1) ? pica.fixed_attrib[k] : (Vec4){ 0, 0, 0, 1 };
    for (int b = 0; b < 12; b++) {
        uint32_t off = pica.reg[REG_ATTRIBBUFFER0_OFFSET + 3 * b] & 0xFFFFFFF;
        uint32_t cfg1 = pica.reg[REG_ATTRIBBUFFER0_OFFSET + 3 * b + 1];
        uint32_t cfg2 = pica.reg[REG_ATTRIBBUFFER0_OFFSET + 3 * b + 2];
        uint32_t ncomp = cfg2 >> 28, stride = cfg2 >> 16 & 0xFF;
        if (!ncomp)
            continue;
        uint32_t vaddr = gpu_phys_to_virt(base + off);
        if (!vaddr)
            gpu_fatal("vertex array %d at physical %08" PRIx32 " is unmapped", b, base + off);
        const uint8_t *p = gpu_mem(vaddr + index * stride, stride);
        uint32_t pos = 0;
        for (uint32_t c = 0; c < ncomp; c++) {
            uint32_t comp = c < 8 ? cfg1 >> (4 * c) & 0xF : cfg2 >> (4 * (c - 8)) & 0xF;
            if (comp >= 12) {               /* padding: 4, 8, 12 or 16 bytes */
                pos += 4 * (comp - 11);
                continue;
            }
            uint32_t fmt = attrib_format(comp), type = fmt & 3, count = (fmt >> 2) + 1;
            uint32_t size = type == 3 ? 4 : type == 2 ? 2 : 1;
            pos = (pos + size - 1) / size * size;   /* elements are naturally aligned */
            if (pos + size * count > stride)
                gpu_fatal("vertex array %d: attribute %" PRIu32 " runs past the %" PRIu32 "-byte stride", b, comp, stride);
            float v[4] = { 0, 0, 0, 1 };
            for (uint32_t e = 0; e < count; e++)
                v[e] = fetch(p + pos + e * size, type);
            attr[comp] = (Vec4){ v[0], v[1], v[2], v[3] };
            pos += size * count;
        }
    }
}

/* Run the vertex shader on one vertex's attributes and gather its outputs
   by semantic. */
/* Map a shader's output registers to vertex attributes: the n-th register
   enabled in `mask` is described by SH_OUTMAP_On. */
static void outputs_to_vertex(const ShaderRegs *s, uint32_t mask, Vertex *out)
{
    for (int i = 0; i < NSEM; i++)
        out->a[i] = 0;
    out->a[SEM_POS + 3] = 1;
    uint32_t n = 0, total = pica.reg[REG_SH_OUTMAP_TOTAL] & 7;
    for (int r = 0; r < 16 && n < total; r++) {
        if (!(mask >> r & 1))
            continue;
        uint32_t map = pica.reg[REG_SH_OUTMAP_O0 + n++];
        float o[4] = { s->o[r].x, s->o[r].y, s->o[r].z, s->o[r].w };
        for (int c = 0; c < 4; c++) {
            uint32_t sem = map >> (8 * c) & 0x1F;
            if (sem < NSEM)
                out->a[sem] = o[c];
        }
    }
}

static void shade(const Vec4 attr[12], ShaderRegs *regs_out, Vertex *out)
{
    const uint32_t *vsh = &pica.reg[REG_VSH_BASE];
    ShaderRegs s;
    memset(&s, 0, sizeof s);
    uint32_t ninputs = (vsh[SH_INPUTBUFFER_CONFIG] & 0xF) + 1;
    uint64_t perm = (uint64_t)vsh[SH_ATTRIBUTES_PERMUTATION_HIGH] << 32 | vsh[SH_ATTRIBUTES_PERMUTATION_LOW];
    for (uint32_t k = 0; k < ninputs && k < 12; k++)
        s.v[perm >> (4 * k) & 0xF] = attr[k];
    uint8_t ints[4][4];
    for (int i = 0; i < 4; i++)
        memcpy(ints[i], &vsh[SH_INTUNIFORM_I0 + i], 4);
    shader_exec(&pica.vsh, vsh[SH_ENTRYPOINT] & 0xFFFF, (uint16_t)vsh[SH_BOOLUNIFORM], ints, &s);
    if (regs_out)
        *regs_out = s;
    if (out)
        outputs_to_vertex(&s, vsh[SH_OUTMAP_MASK], out);
}

/* ---------------------------------------------------------------- clipping */

typedef struct { float x, y, z, w; Vertex v; } ClipVertex;

static ClipVertex lerp_vertex(const ClipVertex *a, const ClipVertex *b, float t)
{
    ClipVertex r;
    for (int i = 0; i < NSEM; i++)
        r.v.a[i] = a->v.a[i] + (b->v.a[i] - a->v.a[i]) * t;
    r.x = r.v.a[0];
    r.y = r.v.a[1];
    r.z = r.v.a[2];
    r.w = r.v.a[3];
    return r;
}

/* Distance to each clip plane, >= 0 inside: x in [-w, w], y in [-w, w],
   z in [-w, 0], and w > 0 (a small epsilon keeps 1/w finite). */
static float plane_dist(const ClipVertex *v, int p)
{
    switch (p) {
    case 0:  return v->w - v->x;
    case 1:  return v->w + v->x;
    case 2:  return v->w - v->y;
    case 3:  return v->w + v->y;
    case 4:  return -v->z;
    case 5:  return v->w + v->z;
    default: return v->w - 1e-6f;
    }
}

/* Sutherland-Hodgman against the 7 planes; returns the vertex count. */
static int clip_polygon(ClipVertex *poly, int n)
{
    ClipVertex tmp[16];
    for (int p = 0; p < 7 && n; p++) {
        int m = 0;
        for (int i = 0; i < n; i++) {
            const ClipVertex *a = &poly[i], *b = &poly[(i + 1) % n];
            float da = plane_dist(a, p), db = plane_dist(b, p);
            if (da >= 0)
                tmp[m++] = *a;
            if ((da >= 0) != (db >= 0))
                tmp[m++] = lerp_vertex(a, b, da / (da - db));
        }
        n = m;
        memcpy(poly, tmp, (size_t)n * sizeof *poly);
    }
    return n;
}

/* ---------------------------------------------------------------- framebuffer */

typedef struct {
    uint8_t *color, *depth;
    uint32_t width, height;
    int color_fmt, depth_fmt;           /* pixel format (gpu.h); depth: 0 = 16, 2 = 24, 3 = 24 + 8 stencil */
    int color_read, color_write, depth_read, depth_write, stencil_read, stencil_write;
} Target;

static int color_format(uint32_t reg)
{
    switch (reg >> 16 & 7) {
    case 0:  return PIXEL_RGBA8;
    case 2:  return PIXEL_RGB5A1;
    case 3:  return PIXEL_RGB565;
    case 4:  return PIXEL_RGBA4;
    default: gpu_fatal("colour buffer format %08" PRIx32, reg);
    }
}

static void get_target(Target *t)
{
    uint32_t dim = pica.reg[REG_FRAMEBUFFER_DIM];
    t->width = dim & 0x7FF;
    t->height = (dim >> 12 & 0x3FF) + 1;
    if (pica.reg[REG_FRAMEBUFFER_BLOCK32] & 1)
        gpu_fatal("32x32-block framebuffers are not implemented");
    t->color_fmt = color_format(pica.reg[REG_COLORBUFFER_FORMAT]);
    t->depth_fmt = (int)(pica.reg[REG_DEPTHBUFFER_FORMAT] & 3);
    t->color_read = (pica.reg[REG_COLORBUFFER_READ] & 0xF) != 0;
    t->color_write = (pica.reg[REG_COLORBUFFER_WRITE] & 0xF) != 0;
    t->stencil_read = pica.reg[REG_DEPTHBUFFER_READ] & 1;
    t->depth_read = pica.reg[REG_DEPTHBUFFER_READ] >> 1 & 1;
    t->stencil_write = pica.reg[REG_DEPTHBUFFER_WRITE] & 1;
    t->depth_write = pica.reg[REG_DEPTHBUFFER_WRITE] >> 1 & 1;
    uint32_t npix = t->width * t->height;
    t->color = NULL;
    t->depth = NULL;
    if (t->color_read || t->color_write) {
        uint32_t v = gpu_phys_to_virt(pica.reg[REG_COLORBUFFER_LOC] << 3);
        if (!v)
            gpu_fatal("colour buffer at physical %08" PRIx32, pica.reg[REG_COLORBUFFER_LOC] << 3);
        t->color = gpu_mem(v, npix * (uint32_t)pixel_size(t->color_fmt));
    }
    if (t->depth_read || t->depth_write || t->stencil_read || t->stencil_write) {
        uint32_t v = gpu_phys_to_virt(pica.reg[REG_DEPTHBUFFER_LOC] << 3);
        if (!v)
            gpu_fatal("depth buffer at physical %08" PRIx32, pica.reg[REG_DEPTHBUFFER_LOC] << 3);
        t->depth = gpu_mem(v, npix * (t->depth_fmt == 0 ? 2u : t->depth_fmt == 2 ? 3u : 4u));
    }
}

/* ---------------------------------------------------------------- colour combiners */

typedef struct { int r, g, b, a; } Color;     /* 0..255 */

/* What the colour combiners can read for one fragment: the interpolated
   vertex colour and texture coordinates (with their screen-space
   derivatives, for mipmapping); textures are sampled when first used. */
typedef struct {
    Color primary;
    float tc[3][2], tc0w;
    float d[3][4];                  /* per coordinate set: du/dx, dv/dx, du/dy, dv/dy */
    Color tex[3];
    int sampled;                    /* bit i: tex[i] is valid */
    float quat[4], view[3];         /* for fragment lighting */
    Color light[2];                 /* fragment primary / secondary colour */
    int lit;
} Frag;

static const uint32_t tev_base[6] = { 0x0C0, 0x0C8, 0x0D0, 0x0D8, 0x0F0, 0x0F8 };

static Color unpack_color(uint32_t v)
{
    return (Color){ (int)(v & 0xFF), (int)(v >> 8 & 0xFF), (int)(v >> 16 & 0xFF), (int)(v >> 24) };
}

static int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* What a combiner reads from a texture unit or fragment lighting that is
   off: zero, a guess (3dbrew doesn't say). GPU_UNSET=white reads white
   instead, to compare the two on a scene. */
static Color unset_color(void)
{
    static int white = -1;
    if (white < 0) {
        const char *e = getenv("GPU_UNSET");
        white = e && strcmp(e, "white") == 0;
    }
    return white ? (Color){ 255, 255, 255, 255 } : (Color){ 0, 0, 0, 0 };
}

/* Sample texture unit `unit` for this fragment (once). */
static Color frag_texture(Frag *f, int unit)
{
    if (f->sampled >> unit & 1)
        return f->tex[unit];
    uint32_t config = pica.reg[REG_TEXUNIT_CONFIG];
    TexUnit t = { 0 };
    if (config >> unit & 1)
        tex_unit_read(unit, &t);
    if (!t.width || !t.height) {
        /* A unit that is disabled, or enabled with no size set: Pokémon X's
           intro clears all three units and then draws with a combiner that
           reads them. 3dbrew doesn't say what the hardware samples then. */
        static int told[3];
        if (!told[unit]++)
            fprintf(stderr, "gpu: a colour combiner reads texture %d, which is %s %" PRIu32 "x%" PRIu32
                    " (address %08" PRIx32 "): reading zero\n", unit,
                    config >> unit & 1 ? "enabled," : "disabled,", t.width, t.height, t.addr[0]);
        f->tex[unit] = unset_color();
        f->sampled |= 1 << unit;
        return f->tex[unit];
    }
    /* Unit 2 can use texture coordinates 2 or 1. */
    int set = unit == 2 && (config >> 13 & 1) ? 1 : unit;
    const float *d = f->d[set];
    float sx = hypotf(d[0] * (float)t.width, d[1] * (float)t.height);
    float sy = hypotf(d[2] * (float)t.width, d[3] * (float)t.height);
    float scale = fmaxf(sx, sy), lod = scale > 0 ? log2f(scale) : -100.0f;
    Rgba c = tex_sample(&t, f->tc[set][0], f->tc[set][1], f->tc0w, lod);
    f->tex[unit] = (Color){ c.r, c.g, c.b, c.a };
    f->sampled |= 1 << unit;
    return f->tex[unit];
}

/* GPU_STATS=1: where triangles and pixels go, reported once a second
   (gpu_stats_report, from the vblank): drawn, clipped away, culled, and the
   fragments that fail the alpha, stencil or depth test or get written. */
typedef struct { uint64_t tris, clipped, culled, frags, alpha, stencil, depth, written, unlit; } Stats;
static Stats stats;
/* Rasterising runs on several threads (see raster_flush): each counts into
   its own Stats, added to `stats` when its band is done. */
static _Thread_local Stats *band_stats = &stats;

/* Fragment lighting's primary (0) or secondary (1) colour. With lighting
   disabled (3dbrew doesn't say what the combiners then read), a unit that
   is off adds no light: both colours are zero. */
static Color frag_light(Frag *f, int which)
{
    if (!f->lit && !(pica.reg[REG_LIGHTING_ENABLE0] & 1)) {
        static int told;
        if (!told++)
            fprintf(stderr, "gpu: a colour combiner reads fragment lighting while it is disabled: reading zero\n");
        f->light[0] = f->light[1] = unset_color();
        f->lit = 1;
        band_stats->unlit++;
    }
    if (!f->lit) {
        uint32_t c0 = pica.reg[0x1C3];
        Rgba bump, p, s;
        int bumped = (c0 >> 28 & 3) != 0;
        if (bumped) {
            Color t = frag_texture(f, (int)(c0 >> 22 & 3));
            bump = (Rgba){ (uint8_t)t.r, (uint8_t)t.g, (uint8_t)t.b, (uint8_t)t.a };
        }
        lighting_compute(f->quat, f->view, bumped ? &bump : NULL, &p, &s);
        f->light[0] = (Color){ p.r, p.g, p.b, p.a };
        f->light[1] = (Color){ s.r, s.g, s.b, s.a };
        f->lit = 1;
    }
    return f->light[which];
}

/* Texture 3: the procedural texture, on texture coordinates 0, 1 or 2. */
static Color frag_proctex(Frag *f)
{
    uint32_t config = pica.reg[REG_TEXUNIT_CONFIG];
    if (!(config >> 10 & 1))
        gpu_fatal("a colour combiner reads the procedural texture, which is disabled");
    uint32_t set = config >> 8 & 3;
    if (set > 2)
        gpu_fatal("procedural texture coordinates %" PRIu32, set);
    Rgba c = proctex_sample(f->tc[set][0], f->tc[set][1]);
    return (Color){ c.r, c.g, c.b, c.a };
}

static Color tev_source(uint32_t src, Frag *f, Color prev, Color buffer, Color constant, int stage)
{
    switch (src) {
    case 0:  return f->primary;
    case 13: return buffer;
    case 14: return constant;
    case 15: return stage ? prev : (Color){ 0, 0, 0, 0 };
    case 1:
    case 2:  return frag_light(f, (int)src - 1);
    case 3: case 4: case 5:
        return frag_texture(f, (int)src - 3);
    case 6:
        return frag_proctex(f);
    default: gpu_fatal("colour combiner %d source %" PRIu32, stage, src);
    }
}

static int rgb_operand(Color c, uint32_t op, int ch)   /* ch: 0 = r, 1 = g, 2 = b */
{
    int v;
    switch (op >> 1) {
    case 0:  v = ch == 0 ? c.r : ch == 1 ? c.g : c.b; break;   /* colour */
    case 1:  v = c.a; break;
    case 2:  v = c.r; break;
    case 4:  v = c.g; break;
    case 6:  v = c.b; break;
    default: gpu_fatal("colour combiner RGB operand %" PRIu32, op);
    }
    return op & 1 ? 255 - v : v;
}

static int alpha_operand(Color c, uint32_t op)
{
    int v;
    switch (op >> 1) {
    case 0:  v = c.a; break;
    case 1:  v = c.r; break;
    case 2:  v = c.g; break;
    case 3:  v = c.b; break;
    default: gpu_fatal("colour combiner alpha operand %" PRIu32, op);
    }
    return op & 1 ? 255 - v : v;
}

static int combine(uint32_t mode, int a, int b, int c)
{
    switch (mode) {
    case 0:  return a;                                          /* replace */
    case 1:  return a * b / 255;                                /* modulate */
    case 2:  return clamp255(a + b);                            /* add */
    case 3:  return clamp255(a + b - 128);                      /* add signed */
    case 4:  return (a * c + b * (255 - c)) / 255;              /* interpolate */
    case 5:  return clamp255(a - b);                            /* subtract */
    case 8:  return clamp255(a * b / 255 + c);                  /* multiply then add */
    case 9:  return clamp255(a + b) * c / 255;                  /* add then multiply */
    default: gpu_fatal("colour combine mode %" PRIu32, mode);
    }
}

static int sources_used(uint32_t mode)
{
    return mode == 0 ? 1 : (mode == 4 || mode == 8 || mode == 9) ? 3 : 2;
}

static Color run_combiners(Frag *f)
{
    Color prev = { 0, 0, 0, 0 }, buffer = unpack_color(pica.reg[REG_TEXENV_BUFFER_COLOR]);
    uint32_t update = pica.reg[REG_TEXENV_UPDATE_BUFFER];
    for (int i = 0; i < 6; i++) {
        const uint32_t *r = &pica.reg[tev_base[i]];
        uint32_t source = r[0], operand = r[1], comb = r[2], scale = r[4];
        Color constant = unpack_color(r[3]);
        uint32_t rgb_mode = comb & 0xF, alpha_mode = comb >> 16 & 0xF;
        Color in[3], out;
        int nr = sources_used(rgb_mode), na = sources_used(alpha_mode);
        if (rgb_mode == 6 || rgb_mode == 7) nr = 2;
        if (alpha_mode == 6 || alpha_mode == 7) na = 2;

        int rgb[3][3], alpha[3];
        for (int k = 0; k < 3; k++) {
            if (k < nr) {
                in[k] = tev_source(source >> (4 * k) & 0xF, f, prev, buffer, constant, i);
                for (int ch = 0; ch < 3; ch++)
                    rgb[k][ch] = rgb_operand(in[k], operand >> (4 * k) & 0xF, ch);
            }
            if (k < na)
                alpha[k] = alpha_operand(tev_source(source >> (16 + 4 * k) & 0xF, f, prev, buffer, constant, i),
                                         operand >> (12 + 4 * k) & 7);
        }
        if (rgb_mode == 6 || rgb_mode == 7) {       /* dot3: 4 * sum((a - 0.5)(b - 0.5)) */
            int d = 0;
            for (int ch = 0; ch < 3; ch++)
                d += (rgb[0][ch] - 128) * (rgb[1][ch] - 128);
            d = clamp255(d * 4 / 255);
            out.r = out.g = out.b = d;
            out.a = rgb_mode == 7 ? d : combine(alpha_mode, alpha[0], na > 1 ? alpha[1] : 0, na > 2 ? alpha[2] : 0);
        } else {
            out.r = combine(rgb_mode, rgb[0][0], nr > 1 ? rgb[1][0] : 0, nr > 2 ? rgb[2][0] : 0);
            out.g = combine(rgb_mode, rgb[0][1], nr > 1 ? rgb[1][1] : 0, nr > 2 ? rgb[2][1] : 0);
            out.b = combine(rgb_mode, rgb[0][2], nr > 1 ? rgb[1][2] : 0, nr > 2 ? rgb[2][2] : 0);
            if (alpha_mode == 6 || alpha_mode == 7)
                gpu_fatal("dot3 in the alpha combiner");
            out.a = combine(alpha_mode, alpha[0], na > 1 ? alpha[1] : 0, na > 2 ? alpha[2] : 0);
        }
        int rs = 1 << (scale & 3), as = 1 << (scale >> 16 & 3);
        out.r = clamp255(out.r * rs);
        out.g = clamp255(out.g * rs);
        out.b = clamp255(out.b * rs);
        out.a = clamp255(out.a * as);

        /* The buffer the next stage can read: stages 0-3's results feed it
           when their update bit is set. */
        Color next_buffer = buffer;
        if (i < 4) {
            if (update >> (8 + i) & 1) {
                next_buffer.r = out.r;
                next_buffer.g = out.g;
                next_buffer.b = out.b;
            }
            if (update >> (12 + i) & 1)
                next_buffer.a = out.a;
        }
        buffer = next_buffer;
        prev = out;
    }
    return prev;
}

/* ---------------------------------------------------------------- fragment tests and blending */

static int test(uint32_t func, uint32_t a, uint32_t b)
{
    switch (func) {
    case 0:  return 0;
    case 1:  return 1;
    case 2:  return a == b;
    case 3:  return a != b;
    case 4:  return a < b;
    case 5:  return a <= b;
    case 6:  return a > b;
    default: return a >= b;
    }
}

static int blend_factor(uint32_t f, Color src, Color dst, Color k, int ch)  /* ch 3 = alpha */
{
    int s = ch == 0 ? src.r : ch == 1 ? src.g : ch == 2 ? src.b : src.a;
    int d = ch == 0 ? dst.r : ch == 1 ? dst.g : ch == 2 ? dst.b : dst.a;
    int c = ch == 0 ? k.r : ch == 1 ? k.g : ch == 2 ? k.b : k.a;
    switch (f) {
    case 0:  return 0;
    case 1:  return 255;
    case 2:  return s;
    case 3:  return 255 - s;
    case 4:  return d;
    case 5:  return 255 - d;
    case 6:  return src.a;
    case 7:  return 255 - src.a;
    case 8:  return dst.a;
    case 9:  return 255 - dst.a;
    case 10: return c;
    case 11: return 255 - c;
    case 12: return k.a;
    case 13: return 255 - k.a;
    case 14: return ch == 3 ? 255 : (src.a < 255 - dst.a ? src.a : 255 - dst.a);
    default: gpu_fatal("blend factor %" PRIu32, f);
    }
}

static int blend_eq(uint32_t eq, int s, int d)
{
    switch (eq) {
    case 1:  return clamp255(s - d);
    case 2:  return clamp255(d - s);
    case 3:  return s < d ? s : d;
    case 4:  return s > d ? s : d;
    default: return clamp255(s + d);        /* 0 (and 5-7 behave like add) */
    }
}

static uint32_t logic_op(uint32_t op, uint32_t s, uint32_t d)
{
    switch (op) {
    case 0:  return 0;
    case 1:  return s & d;
    case 2:  return s & ~d;
    case 3:  return s;
    case 4:  return 0xFF;
    case 5:  return ~s;
    case 6:  return d;
    case 7:  return ~d;
    case 8:  return ~(s & d);
    case 9:  return s | d;
    case 10: return ~(s | d);
    case 11: return s ^ d;
    case 12: return ~(s ^ d);
    case 13: return ~s & d;
    case 14: return s | ~d;
    default: return ~s | d;
    }
}

static uint32_t stencil_op(uint32_t op, uint32_t old, uint32_t ref)
{
    switch (op) {
    case 0:  return old;
    case 1:  return 0;
    case 2:  return ref;
    case 3:  return old < 255 ? old + 1 : 255;
    case 4:  return old > 0 ? old - 1 : 0;
    case 5:  return ~old & 0xFF;
    case 6:  return (old + 1) & 0xFF;
    default: return (old - 1) & 0xFF;
    }
}

/* Fog: a 128-entry table indexed by the fragment's depth (or 1 - depth
   with the "Z flip" bit) gives how much of the colour survives; the rest
   is the fog colour. Each entry: an 11-bit value (bits 13-23) and a signed
   13-bit difference to the next entry (bits 0-12), both in 1/2048ths. */
static Color apply_fog(Color c, float depth)
{
    float d = depth < 0 ? 0 : depth > 1 ? 1 : depth;
    if (pica.reg[REG_TEXENV_UPDATE_BUFFER] >> 16 & 1)
        d = 1 - d;
    float x = d * 128;
    int i = x >= 127 ? 127 : (int)x;
    uint32_t e = pica.fog_lut[i];
    float value = (float)(e >> 13 & 0x7FF) / 2048, diff = (float)((int32_t)(e << 19) >> 19) / 2048;
    float k = value + diff * (x - (float)i);
    k = k < 0 ? 0 : k > 1 ? 1 : k;
    uint32_t fc = pica.reg[REG_FOG_COLOR];
    int fr = (int)(fc & 0xFF), fg = (int)(fc >> 8 & 0xFF), fb = (int)(fc >> 16 & 0xFF);
    c.r = (int)lroundf((float)c.r * k + (float)fr * (1 - k));
    c.g = (int)lroundf((float)c.g * k + (float)fg * (1 - k));
    c.b = (int)lroundf((float)c.b * k + (float)fb * (1 - k));
    return c;
}

/* One fragment at render-target pixel (x, y) (y measured from the bottom). */
void gpu_stats_report(uint32_t frame)
{
    static int on = -1;
    if (on < 0)
        on = getenv("GPU_STATS") != NULL;
    if (!on || frame % 60)
        return;
    fprintf(stderr, "gpu stats (frame %u): %llu triangles (%llu clipped away, %llu culled), %llu fragments: "
            "%llu failed alpha, %llu stencil, %llu depth; %llu written (%llu read lighting while off)\n", frame,
            (unsigned long long)stats.tris, (unsigned long long)stats.clipped, (unsigned long long)stats.culled,
            (unsigned long long)stats.frags, (unsigned long long)stats.alpha, (unsigned long long)stats.stencil,
            (unsigned long long)stats.depth, (unsigned long long)stats.written, (unsigned long long)stats.unlit);
    memset(&stats, 0, sizeof stats);
}

static void fragment(const Target *t, uint32_t x, uint32_t y, float depth, Frag *f)
{
    band_stats->frags++;
    /* Memory rows go top to bottom; the window's y = 0 is the bottom row. */
    uint32_t idx = tile_index(x, t->height - 1 - y, t->width);

    Color c = run_combiners(f);
    if ((pica.reg[REG_TEXENV_UPDATE_BUFFER] & 7) == 5)
        c = apply_fog(c, depth);

    uint32_t at = pica.reg[REG_ALPHA_TEST];
    if ((at & 1) && !test(at >> 4 & 7, (uint32_t)c.a, at >> 8 & 0xFF)) {
        band_stats->alpha++;
        return;
    }

    /* depth and stencil */
    uint32_t dmax = t->depth_fmt == 0 ? 0xFFFF : 0xFFFFFF;
    float dz = depth < 0 ? 0 : depth > 1 ? 1 : depth;
    uint32_t z = (uint32_t)(dz * (float)dmax), old_z = 0, stencil = 0;
    uint8_t *dp = NULL;
    if (t->depth) {
        uint32_t bytes = t->depth_fmt == 0 ? 2 : t->depth_fmt == 2 ? 3 : 4;
        dp = t->depth + idx * bytes;
        uint32_t raw = 0;
        memcpy(&raw, dp, bytes);
        old_z = raw & dmax;
        stencil = t->depth_fmt == 3 ? raw >> 24 : 0;
    }
    uint32_t st = pica.reg[REG_STENCIL_TEST], so = pica.reg[REG_STENCIL_OP];
    int stencil_on = (st & 1) && t->depth_fmt == 3 && dp;
    uint32_t sref = st >> 16 & 0xFF, smask = st >> 24, swmask = st >> 8 & 0xFF, new_stencil = stencil;
    if (stencil_on && !test(st >> 4 & 7, sref & smask, stencil & smask)) {
        band_stats->stencil++;
        new_stencil = stencil_op(so & 7, stencil, sref);
        goto write_stencil;
    }
    uint32_t dcm = pica.reg[REG_DEPTH_COLOR_MASK];
    int depth_pass = !(dcm & 1) || !t->depth_read || test(dcm >> 4 & 7, z, old_z);
    if (!depth_pass) {
        band_stats->depth++;
        if (stencil_on)
            new_stencil = stencil_op(so >> 4 & 7, stencil, sref);
        goto write_stencil;
    }
    if (stencil_on)
        new_stencil = stencil_op(so >> 8 & 7, stencil, sref);
    if (dp && t->depth_write && (dcm >> 12 & 1)) {
        uint32_t bytes = t->depth_fmt == 0 ? 2 : 3;
        memcpy(dp, &z, bytes);
    }

    /* blend / logic op, then the colour write mask */
    if (t->color && t->color_write) {
        band_stats->written++;
        uint8_t *cp = t->color + idx * (uint32_t)pixel_size(t->color_fmt);
        Rgba d8 = color_decode(cp, t->color_fmt);
        Color dst = { d8.r, d8.g, d8.b, d8.a }, out = c;
        if (pica.reg[REG_COLOR_OPERATION] >> 8 & 1) {
            uint32_t bf = pica.reg[REG_BLEND_FUNC];
            Color k = unpack_color(pica.reg[REG_BLEND_COLOR]);
            for (int ch = 0; ch < 4; ch++) {
                int alpha = ch == 3;
                uint32_t eq = alpha ? bf >> 8 & 7 : bf & 7;
                uint32_t sf = alpha ? bf >> 24 & 0xF : bf >> 16 & 0xF, df = alpha ? bf >> 28 : bf >> 20 & 0xF;
                int s = ch == 0 ? c.r : ch == 1 ? c.g : ch == 2 ? c.b : c.a;
                int dv = ch == 0 ? dst.r : ch == 1 ? dst.g : ch == 2 ? dst.b : dst.a;
                int v = blend_eq(eq, s * blend_factor(sf, c, dst, k, ch) / 255, dv * blend_factor(df, c, dst, k, ch) / 255);
                if (ch == 0) out.r = v; else if (ch == 1) out.g = v; else if (ch == 2) out.b = v; else out.a = v;
            }
        } else {
            uint32_t op = pica.reg[REG_LOGIC_OP] & 0xF;
            out.r = (int)(logic_op(op, (uint32_t)c.r, (uint32_t)dst.r) & 0xFF);
            out.g = (int)(logic_op(op, (uint32_t)c.g, (uint32_t)dst.g) & 0xFF);
            out.b = (int)(logic_op(op, (uint32_t)c.b, (uint32_t)dst.b) & 0xFF);
            out.a = (int)(logic_op(op, (uint32_t)c.a, (uint32_t)dst.a) & 0xFF);
        }
        Rgba w = {
            (uint8_t)((dcm >> 8 & 1) ? out.r : dst.r), (uint8_t)((dcm >> 9 & 1) ? out.g : dst.g),
            (uint8_t)((dcm >> 10 & 1) ? out.b : dst.b), (uint8_t)((dcm >> 11 & 1) ? out.a : dst.a),
        };
        color_encode(cp, t->color_fmt, w);
    }

write_stencil:
    if (stencil_on && t->stencil_write && dp) {
        uint8_t s8 = (uint8_t)((stencil & ~swmask) | (new_stencil & swmask));
        dp[3] = s8;
    }
}

/* ---------------------------------------------------------------- rasterisation */

static void check_unsupported(void)
{
    uint32_t fog = pica.reg[REG_TEXENV_UPDATE_BUFFER] & 7;
    if (fog && fog != 5)
        gpu_fatal("%s is not implemented yet", fog == 7 ? "gas" : "fog/gas mode");
    if (pica.reg[REG_COLOR_OPERATION] & 3)
        gpu_fatal("fragment operation mode %" PRIu32 " (gas/shadow) is not implemented yet",
                  pica.reg[REG_COLOR_OPERATION] & 3);
    if (pica.reg[REG_FRAGOP_CLIP] & 1)
        gpu_fatal("user clip planes are not implemented yet");
}

static float edge(float ax, float ay, float bx, float by, float px, float py)
{
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

/* Top-left fill rule: a pixel exactly on an edge belongs to the triangle
   only for top edges and left edges, so shared edges are drawn once. */
static int top_left(float ax, float ay, float bx, float by)
{
    return (ay == by && bx < ax) || by < ay;
}

/* The attributes at window position (px, py), perspective-correct:
   interpolate a/w and 1/w linearly, then divide. */
static void interpolate(const WinVertex *v0, const WinVertex *v1, const WinVertex *v2, float area,
                        float px, float py, float out[NSEM])
{
    float b0 = edge(v1->x, v1->y, v2->x, v2->y, px, py) / area;
    float b1 = edge(v2->x, v2->y, v0->x, v0->y, px, py) / area;
    float b2 = 1.0f - b0 - b1;
    float inv_w = b0 * v0->inv_w + b1 * v1->inv_w + b2 * v2->inv_w;
    for (int k = 0; k < NSEM; k++)
        out[k] = (b0 * v0->a[k] + b1 * v1->a[k] + b2 * v2->a[k]) / inv_w;
}

/* Rows go to bands in stripes of 8: band b of n draws the rows whose stripe
   number is b modulo n (all rows when n is 1). */
#define BAND_ROWS 8

/* interpolate() for the texture coordinates only (sets 0-2, u and v each),
   the same arithmetic: the neighbours' values, for the derivatives. */
static const int tc_slots[6] = { SEM_TC0, SEM_TC0 + 1, SEM_TC1, SEM_TC1 + 1, SEM_TC2, SEM_TC2 + 1 };

static void interpolate_tc(const WinVertex *v0, const WinVertex *v1, const WinVertex *v2, float area,
                           float px, float py, float out[6])
{
    float b0 = edge(v1->x, v1->y, v2->x, v2->y, px, py) / area;
    float b1 = edge(v2->x, v2->y, v0->x, v0->y, px, py) / area;
    float b2 = 1.0f - b0 - b1;
    float inv_w = b0 * v0->inv_w + b1 * v1->inv_w + b2 * v2->inv_w;
    for (int k = 0; k < 6; k++) {
        int s = tc_slots[k];
        out[k] = (b0 * v0->a[s] + b1 * v1->a[s] + b2 * v2->a[s]) / inv_w;
    }
}

static void raster_triangle(const Target *t, const WinVertex *v0, const WinVertex *v1, const WinVertex *v2,
                            int band, int nbands)
{
    float area = edge(v0->x, v0->y, v1->x, v1->y, v2->x, v2->y);
    if (area == 0)
        return;

    /* face culling: counter-clockwise (positive area, y up) is the front */
    uint32_t cull = pica.reg[REG_FACECULLING] & 3;
    if ((cull == 1 && area > 0) || (cull == 2 && area < 0)) {
        band_stats->culled++;
        return;
    }
    if (area < 0) {                     /* make it counter-clockwise for the edge tests */
        const WinVertex *tmp = v1;
        v1 = v2;
        v2 = tmp;
        area = -area;
    }

    /* bounding box, clipped to the target and the scissor box */
    float minx = fminf(v0->x, fminf(v1->x, v2->x)), maxx = fmaxf(v0->x, fmaxf(v1->x, v2->x));
    float miny = fminf(v0->y, fminf(v1->y, v2->y)), maxy = fmaxf(v0->y, fmaxf(v1->y, v2->y));
    int x0 = (int)floorf(minx), x1 = (int)ceilf(maxx), y0 = (int)floorf(miny), y1 = (int)ceilf(maxy);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)t->width) x1 = (int)t->width;
    if (y1 > (int)t->height) y1 = (int)t->height;
    uint32_t smode = pica.reg[REG_SCISSOR_MODE] & 3, spos = pica.reg[REG_SCISSOR_POS], sdim = pica.reg[REG_SCISSOR_DIM];
    int sx0 = (int)(spos & 0x3FF), sy0 = (int)(spos >> 16 & 0x3FF), sx1 = (int)(sdim & 0x3FF), sy1 = (int)(sdim >> 16 & 0x3FF);

    int tl0 = top_left(v1->x, v1->y, v2->x, v2->y), tl1 = top_left(v2->x, v2->y, v0->x, v0->y),
        tl2 = top_left(v0->x, v0->y, v1->x, v1->y);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            if (nbands > 1 && (y / BAND_ROWS) % nbands != band) {
                x = x1;                 /* not this band's row */
                continue;
            }
            if (smode == 3 && (x < sx0 || x > sx1 || y < sy0 || y > sy1))
                continue;
            if (smode == 1 && x >= sx0 && x <= sx1 && y >= sy0 && y <= sy1)
                continue;
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = edge(v1->x, v1->y, v2->x, v2->y, px, py);
            float w1 = edge(v2->x, v2->y, v0->x, v0->y, px, py);
            float w2 = edge(v0->x, v0->y, v1->x, v1->y, px, py);
            if (w0 < 0 || w1 < 0 || w2 < 0 || (w0 == 0 && !tl0) || (w1 == 0 && !tl1) || (w2 == 0 && !tl2))
                continue;
            float b0 = w0 / area, b1 = w1 / area, b2 = w2 / area;
            float z = b0 * v0->z + b1 * v1->z + b2 * v2->z;            /* depth: linear in screen space */
            float a[NSEM], ax[6], ay[6];
            interpolate(v0, v1, v2, area, px, py, a);
            interpolate_tc(v0, v1, v2, area, px + 1, py, ax);      /* neighbours: texture derivatives */
            interpolate_tc(v0, v1, v2, area, px, py + 1, ay);
            Frag f;
            float col[4];
            for (int ch = 0; ch < 4; ch++) {
                float c = a[SEM_COLOR + ch];
                col[ch] = c < 0 ? 0 : c > 1 ? 1 : c;
            }
            f.primary = (Color){ (int)lroundf(col[0] * 255), (int)lroundf(col[1] * 255),
                                 (int)lroundf(col[2] * 255), (int)lroundf(col[3] * 255) };
            for (int k = 0; k < 3; k++) {
                int s = tc_slots[2 * k];
                f.tc[k][0] = a[s];
                f.tc[k][1] = a[s + 1];
                f.d[k][0] = ax[2 * k] - a[s];
                f.d[k][1] = ax[2 * k + 1] - a[s + 1];
                f.d[k][2] = ay[2 * k] - a[s];
                f.d[k][3] = ay[2 * k + 1] - a[s + 1];
            }
            f.tc0w = a[SEM_TC0W];
            f.sampled = 0;
            for (int k = 0; k < 4; k++)
                f.quat[k] = a[SEM_QUAT + k];
            for (int k = 0; k < 3; k++)
                f.view[k] = a[SEM_VIEW + k];
            f.lit = 0;
            fragment(t, (uint32_t)x, (uint32_t)y, z, &f);
        }
}

/* ---------------------------------------------------------------- parallel rasterising */

/* Triangles in window space wait in a queue; raster_flush draws them,
   before any register changes (pica_raster_flush is called from every
   register write) so they all see the state they were drawn with. Each of
   N bands (threads) walks the whole queue in order but draws only its own
   rows, so every pixel gets exactly the single-threaded sequence of
   writes. GPU_THREADS=N sets the number of bands (1: no extra threads). */
typedef struct { Target t; WinVertex v[3]; } QueuedTri;

static QueuedTri *queue;
static size_t nqueue, queue_cap;
static uint64_t queue_pixels;          /* bounding-box area: small batches stay on this thread */
static int nbands = 0;                  /* 0: not set up yet */

static pthread_mutex_t pool_lock = PTHREAD_MUTEX_INITIALIZER, stats_lock = PTHREAD_MUTEX_INITIALIZER;
static void (*pool_job)(int band);      /* what the bands run: rasterising, or shading vertices */
static pthread_cond_t pool_go = PTHREAD_COND_INITIALIZER, pool_idle = PTHREAD_COND_INITIALIZER;
static uint64_t pool_gen;
static int pool_done;

static void stats_add(const Stats *s)
{
    stats.culled += s->culled;
    stats.frags += s->frags;
    stats.alpha += s->alpha;
    stats.stencil += s->stencil;
    stats.depth += s->depth;
    stats.written += s->written;
    stats.unlit += s->unlit;
}

static void raster_band(int band, Stats *s)
{
    band_stats = s;
    for (size_t i = 0; i < nqueue; i++)
        raster_triangle(&queue[i].t, &queue[i].v[0], &queue[i].v[1], &queue[i].v[2], band, nbands);
    band_stats = &stats;
}

static void *raster_worker(void *arg)
{
    int band = (int)(intptr_t)arg;
    uint64_t seen = 0;
    pthread_mutex_lock(&pool_lock);
    for (;;) {
        while (pool_gen == seen)
            pthread_cond_wait(&pool_go, &pool_lock);
        seen = pool_gen;
        pthread_mutex_unlock(&pool_lock);
        pool_job(band);
        pthread_mutex_lock(&pool_lock);
        if (++pool_done == nbands - 1)
            pthread_cond_signal(&pool_idle);
    }
    return NULL;
}

static void pool_start(void)
{
    const char *env = getenv("GPU_THREADS");
    long n = env ? strtol(env, NULL, 0) : sysconf(_SC_NPROCESSORS_ONLN);
    nbands = n < 1 ? 1 : n > 16 ? 16 : (int)n;
    for (int b = 1; b < nbands; b++) {
        pthread_t th;
        if (pthread_create(&th, NULL, raster_worker, (void *)(intptr_t)b) != 0)
            gpu_fatal("cannot start a rasterising thread");
        pthread_detach(th);
    }
}

/* Run `job` on every band, this thread taking band 0, and wait for all. */
static void pool_run(void (*job)(int band))
{
    pthread_mutex_lock(&pool_lock);
    pool_job = job;
    pool_done = 0;
    pool_gen++;
    pthread_cond_broadcast(&pool_go);
    pthread_mutex_unlock(&pool_lock);
    job(0);
    pthread_mutex_lock(&pool_lock);
    while (pool_done < nbands - 1)
        pthread_cond_wait(&pool_idle, &pool_lock);
    pthread_mutex_unlock(&pool_lock);
}

static void raster_job(int band)
{
    Stats s = { 0 };
    raster_band(band, &s);
    pthread_mutex_lock(&stats_lock);
    stats_add(&s);
    pthread_mutex_unlock(&stats_lock);
}

void pica_raster_flush(void)
{
    if (!nqueue)
        return;
    if (!nbands)
        pool_start();
    if (nbands == 1 || queue_pixels < 16384) {
        int all = nbands;               /* one band covering every row */
        nbands = 1;
        Stats s = { 0 };
        raster_band(0, &s);
        stats_add(&s);
        nbands = all;
    } else {
        pool_run(raster_job);
    }
    nqueue = 0;
    queue_pixels = 0;
}

static void raster_queue(const Target *t, const WinVertex *a, const WinVertex *b, const WinVertex *c)
{
    if (nqueue == queue_cap) {
        queue_cap = queue_cap ? 2 * queue_cap : 1024;
        queue = realloc(queue, queue_cap * sizeof *queue);
        if (!queue)
            gpu_fatal("out of memory for the triangle queue");
    }
    QueuedTri *q = &queue[nqueue++];
    q->t = *t;
    q->v[0] = *a;
    q->v[1] = *b;
    q->v[2] = *c;
    float w = fmaxf(a->x, fmaxf(b->x, c->x)) - fminf(a->x, fminf(b->x, c->x));
    float h = fmaxf(a->y, fmaxf(b->y, c->y)) - fminf(a->y, fminf(b->y, c->y));
    queue_pixels += (uint64_t)(fminf(w, 1024) * fminf(h, 1024));
}

static int32_t sign10(uint32_t v)       /* a signed 10-bit field */
{
    return (int32_t)(v << 22) >> 22;
}

/* Clip a triangle, map it to the window and draw it. */
static void draw_triangle(const Vertex *a, const Vertex *b, const Vertex *c)
{
    ClipVertex poly[16];
    const Vertex *in[3] = { a, b, c };
    for (int i = 0; i < 3; i++) {
        poly[i].v = *in[i];
        poly[i].x = in[i]->a[0];
        poly[i].y = in[i]->a[1];
        poly[i].z = in[i]->a[2];
        poly[i].w = in[i]->a[3];
    }
    stats.tris++;
    int n = clip_polygon(poly, 3);
    if (n < 3) {
        stats.clipped++;
        return;
    }

    Target t;
    get_target(&t);
    float half_w = reg_f24(REG_VIEWPORT_WIDTH), half_h = reg_f24(REG_VIEWPORT_HEIGHT);
    uint32_t xy = pica.reg[REG_VIEWPORT_XY];
    float vx = (float)sign10(xy), vy = (float)sign10(xy >> 16);
    float zscale = reg_f24(REG_DEPTHMAP_SCALE), zoffset = reg_f24(REG_DEPTHMAP_OFFSET);
    int zbuffer = pica.reg[REG_DEPTHMAP_ENABLE] & 1;

    WinVertex win[16];
    for (int i = 0; i < n; i++) {
        const ClipVertex *p = &poly[i];
        float inv_w = 1.0f / p->w;
        win[i].x = (p->x * inv_w + 1.0f) * half_w + vx;
        win[i].y = (p->y * inv_w + 1.0f) * half_h + vy;
        win[i].z = (zbuffer ? p->z * inv_w : p->z) * zscale + zoffset;
        win[i].inv_w = inv_w;
        for (int k = 0; k < NSEM; k++)
            win[i].a[k] = p->v.a[k] * inv_w;
    }
    for (int i = 1; i + 1 < n; i++)     /* the clipped polygon is convex: a fan */
        raster_queue(&t, &win[0], &win[i], &win[i + 1]);
}

/* ---------------------------------------------------------------- geometry shaders */

/* Point mode (GSH_MISC0 mode 0): the vertex shader's enabled
   output registers, vertex after vertex, fill the geometry shader's input
   buffer; when it holds GSH_INPUTBUFFER_CONFIG + 1 attributes the
   geometry shader runs once on them (through the input permutation) and
   emits vertices: SETEMIT picks a slot 0-2, EMIT stores the outputs there,
   and an emit flagged "primitive" draws slots 0-2 as a triangle (the
   winding flag swaps two of them). */
static Vec4 gs_input[16];
static uint32_t gs_inputs, gs_mode, gs_vertices;
static uint32_t gs_prim_size;       /* variable mode: vertices in this primitive, 0 = expecting the count */
static Vertex gs_slots[3];

static void gs_emit(ShaderRegs *s)
{
    Vertex *v = &gs_slots[s->emit_vertex % 3];
    outputs_to_vertex(s, pica.reg[REG_GSH_BASE + SH_OUTMAP_MASK], v);
    if (s->emit_prim) {
        if (s->emit_winding)
            draw_triangle(&gs_slots[0], &gs_slots[2], &gs_slots[1]);
        else
            draw_triangle(&gs_slots[0], &gs_slots[1], &gs_slots[2]);
    }
}

static void gs_run(void)
{
    const uint32_t *gsh = &pica.reg[REG_GSH_BASE];
    uint32_t need = (gsh[SH_INPUTBUFFER_CONFIG] & 0xF) + 1;
    ShaderRegs g;
    memset(&g, 0, sizeof g);
    uint64_t perm = (uint64_t)gsh[SH_ATTRIBUTES_PERMUTATION_HIGH] << 32 | gsh[SH_ATTRIBUTES_PERMUTATION_LOW];
    for (uint32_t k = 0; k < need && k < 16; k++)
        g.v[perm >> (4 * k) & 0xF] = gs_input[k];
    uint8_t ints[4][4];
    for (int i = 0; i < 4; i++)
        memcpy(ints[i], &gsh[SH_INTUNIFORM_I0 + i], 4);
    g.on_emit = gs_emit;
    shader_exec(&pica.gsh, gsh[SH_ENTRYPOINT] & 0xFFFF, (uint16_t)gsh[SH_BOOLUNIFORM], ints, &g);
}

/* Fixed-size primitives (GSH_MISC0 mode 2): each vertex's outputs are
   written into the geometry shader's float uniforms, from register
   `start` with `stride` registers per vertex; once `count` vertices are
   in, the geometry shader runs. */
static void gs_feed_fixed(const ShaderRegs *vs)
{
    uint32_t misc = pica.reg[0x252], start = misc >> 16 & 0xFF, stride = (misc >> 12 & 0xF) + 1,
             count = (misc >> 8 & 0xF) + 1;
    uint32_t mask = pica.reg[REG_VSH_BASE + SH_OUTMAP_MASK], k = 0;
    for (int r = 0; r < 16 && k < stride; r++)
        if (mask >> r & 1) {
            uint32_t c = start + gs_vertices * stride + k++;
            if (c < 96)
                pica.gsh.uniform[c] = vs->o[r];
        }
    if (++gs_vertices == count) {
        gs_vertices = 0;
        gs_run();
    }
}

/* Variable-size primitives (GSH_MISC0 mode 1, used for subdivision).
   3dbrew barely documents this mode; the layout here is read off the
   devkitPro loop_subdivision example, whose index list is
   "count, v0 v1 v2, neighbours..." per primitive and whose geometry
   shader reads c0 = count, then 3 registers per main vertex, then one
   register per remaining vertex:
     - the first index of a primitive is not a vertex but the number of
       vertices that follow (gs_begin_variable); it goes into c0;
     - the first GSH_MISC1 + 1 vertices write `stride` output registers
       each from c1 (stride = GSH_INPUTBUFFER_CONFIG + 1);
     - every later vertex writes only its first output register;
     - once all `count` vertices are in, the geometry shader runs. */
static void gs_begin_variable(uint32_t count)
{
    float n = (float)count;
    pica.gsh.uniform[0] = (Vec4){ n, n, n, n };
    gs_prim_size = count;
    gs_vertices = 0;
}

static void gs_feed_variable(const ShaderRegs *vs)
{
    uint32_t main_vertices = (pica.reg[0x254] & 0xFF) + 1,              /* GSH_MISC1 */
             stride = (pica.reg[REG_GSH_BASE + SH_INPUTBUFFER_CONFIG] & 0xF) + 1;
    uint32_t mask = pica.reg[REG_VSH_BASE + SH_OUTMAP_MASK];
    uint32_t c = gs_vertices < main_vertices ? 1 + gs_vertices * stride
                                             : 1 + main_vertices * stride + (gs_vertices - main_vertices);
    uint32_t n = gs_vertices < main_vertices ? stride : 1;
    for (int r = 0; r < 16 && n; r++)
        if (mask >> r & 1) {
            if (c >= 96)
                gpu_fatal("variable-size geometry primitive overflows the float uniforms (%" PRIu32 " vertices)",
                          gs_prim_size);
            pica.gsh.uniform[c++] = vs->o[r];
            n--;
        }
    if (++gs_vertices == gs_prim_size) {
        gs_prim_size = 0;
        gs_run();
    }
}

static void gs_feed(const ShaderRegs *vs)
{
    if (gs_mode == 2) {
        gs_feed_fixed(vs);
        return;
    }
    if (gs_mode == 1) {
        gs_feed_variable(vs);
        return;
    }
    const uint32_t *gsh = &pica.reg[REG_GSH_BASE];
    uint32_t mask = pica.reg[REG_VSH_BASE + SH_OUTMAP_MASK], need = (gsh[SH_INPUTBUFFER_CONFIG] & 0xF) + 1;
    for (int r = 0; r < 16; r++) {
        if (!(mask >> r & 1))
            continue;
        gs_input[gs_inputs++ & 15] = vs->o[r];
        if (gs_inputs < need)
            continue;
        gs_inputs = 0;
        gs_run();
    }
}

static int geometry_shader_on(void)
{
    if ((pica.reg[REG_GEOSTAGE_CONFIG] & 3) != 2)
        return 0;
    gs_mode = pica.reg[0x252] & 0xFF;               /* GSH_MISC0: the processing mode */
    switch (gs_mode) {
    case 0:                                         /* point mode */
    case 1:                                         /* variable-size primitives */
    case 2:                                         /* fixed-size primitives */
        return 1;
    default:
        gpu_fatal("geometry shader mode %" PRIu32, gs_mode);
    }
}

/* ---------------------------------------------------------------- primitive assembly */

static Vertex assembly[3];
static uint32_t assembled;          /* vertices since the primitive (re)started */

void pica_restart_primitive(void)
{
    assembled = 0;
}

static void assemble(const Vertex *v)
{
    uint32_t mode = pica.reg[REG_PRIMITIVE_CONFIG] >> 8 & 3;
    /* Mode 3 ("geometry primitive") without a geometry shader is plain
       triangles: citro3d draws indexed triangles that way, with the
       GEOSTAGE "drawing triangle elements" bit set. */
    if (mode == 3 && (pica.reg[REG_GEOSTAGE_CONFIG] & 3) != 2)
        mode = 0;
    switch (mode) {
    case 0:                         /* triangles */
        assembly[assembled % 3] = *v;
        if (++assembled % 3 == 0)
            draw_triangle(&assembly[0], &assembly[1], &assembly[2]);
        break;
    case 1:                         /* strip: every new vertex makes a triangle with the two before */
        if (assembled >= 2) {
            if (assembled & 1)      /* keep the winding consistent */
                draw_triangle(&assembly[1], &assembly[0], v);
            else
                draw_triangle(&assembly[0], &assembly[1], v);
            assembly[0] = assembly[1];
            assembly[1] = *v;
        } else {
            assembly[assembled] = *v;
        }
        assembled++;
        break;
    case 2:                         /* fan: around the first vertex */
        if (assembled >= 2) {
            draw_triangle(&assembly[0], &assembly[1], v);
            assembly[1] = *v;
        } else {
            assembly[assembled] = *v;
        }
        assembled++;
        break;
    default:
        gpu_fatal("geometry-shader primitives are not implemented yet");
    }
}

/* Shaded vertices by index, for the current indexed draw: a mesh uses
   each vertex in several triangles, and the vertex shader (no side
   effects, its state fixed for the draw) gives the same result every
   time. The GPU keeps a small cache like it; this one keeps more. */
#define VCACHE 4096
static struct { uint32_t gen, index; Vertex v; } vcache[VCACHE];
static uint32_t vcache_gen;

/* A big draw (no geometry shader) shades its distinct vertices on every
   band first, then assembles them in order: the same vertices, the same
   triangles, in the same order, as shading one at a time. */
#define PARALLEL_VERTICES 1024
static uint32_t *job_index, *job_slot;   /* distinct indices; draw position -> their slot */
static Vertex *job_out;
static uint32_t job_n, job_cap;
static struct { uint32_t gen, slot; } index_map[65536];
static uint32_t index_gen;

static void shade_job(int band)
{
    uint32_t lo = (uint32_t)((uint64_t)job_n * (uint32_t)band / (uint32_t)nbands);
    uint32_t hi = (uint32_t)((uint64_t)job_n * (uint32_t)(band + 1) / (uint32_t)nbands);
    for (uint32_t i = lo; i < hi; i++) {
        Vec4 attr[12];
        ShaderRegs s;
        load_attributes(job_index[i], attr);
        shade(attr, &s, &job_out[i]);
    }
}

static void draw_parallel(const uint32_t *indices, uint32_t count, int dedup)
{
    if (count > job_cap) {
        job_cap = count;
        job_index = realloc(job_index, count * sizeof *job_index);
        job_slot = realloc(job_slot, count * sizeof *job_slot);
        job_out = realloc(job_out, count * sizeof *job_out);
        if (!job_index || !job_slot || !job_out)
            gpu_fatal("out of memory for a draw of %" PRIu32 " vertices", count);
    }
    if (dedup && ++index_gen == 0) {
        memset(index_map, 0, sizeof index_map);
        index_gen = 1;
    }
    job_n = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t index = indices[i];
        if (dedup) {
            if (index_map[index].gen == index_gen) {
                job_slot[i] = index_map[index].slot;
                continue;
            }
            index_map[index].gen = index_gen;
            index_map[index].slot = job_n;
        }
        job_slot[i] = job_n;
        job_index[job_n++] = index;
    }
    pool_run(shade_job);
    for (uint32_t i = 0; i < count; i++)
        assemble(&job_out[job_slot[i]]);
}

void pica_draw(int indexed)
{
    if (gpu_skip_draws)
        return;
    check_unsupported();
    int gs = geometry_shader_on();
    gs_inputs = 0;
    gs_vertices = 0;
    gs_prim_size = 0;
    uint32_t count = pica.reg[REG_NUMVERTICES], first = pica.reg[REG_VERTEX_OFFSET];
    uint32_t ib = pica.reg[REG_INDEXBUFFER_CONFIG], base = pica.reg[REG_ATTRIBBUFFERS_LOC] << 3;
    const uint8_t *indices = NULL;
    if (indexed) {
        uint32_t v = gpu_phys_to_virt(base + (ib & 0xFFFFFFF));
        if (!v)
            gpu_fatal("index buffer at physical %08" PRIx32, base + (ib & 0xFFFFFFF));
        indices = gpu_mem(v, count * (ib >> 31 ? 2u : 1u));
    }
    if (gpu_trace)
        fprintf(stderr, "    PICA draw %s: %" PRIu32 " vertices, primitive mode %" PRIu32 "\n",
                indexed ? "elements" : "arrays", count, pica.reg[REG_PRIMITIVE_CONFIG] >> 8 & 3);
    assembled = 0;
    if (!nbands)
        pool_start();
    static uint32_t parallel_min;
    if (!parallel_min) {
        const char *e = getenv("GPU_PARALLEL_MIN");     /* tests set 1, to run this path */
        parallel_min = e ? (uint32_t)strtoul(e, NULL, 0) : PARALLEL_VERTICES;
        if (!parallel_min)
            parallel_min = 1;
    }
    if (!gs && count >= parallel_min && nbands > 1) {
        static uint32_t *all;
        static uint32_t all_cap;
        if (count > all_cap) {
            all_cap = count;
            all = realloc(all, count * sizeof *all);
            if (!all)
                gpu_fatal("out of memory for a draw of %" PRIu32 " vertices", count);
        }
        for (uint32_t i = 0; i < count; i++) {
            if (!indexed) {
                all[i] = first + i;
            } else if (ib >> 31) {
                uint16_t v16;
                memcpy(&v16, indices + 2 * i, 2);
                all[i] = v16;
            } else {
                all[i] = indices[i];
            }
        }
        draw_parallel(all, count, indexed);
        return;
    }
    int cached = indexed && !gs;
    if (cached && ++vcache_gen == 0) {      /* wrapped: forget everything */
        memset(vcache, 0, sizeof vcache);
        vcache_gen = 1;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t index;
        if (!indexed) {
            index = first + i;
        } else if (ib >> 31) {
            uint16_t s;
            memcpy(&s, indices + 2 * i, 2);
            index = s;
        } else {
            index = indices[i];
        }
        if (gs && gs_mode == 1 && gs_prim_size == 0) {
            if (index)                  /* a count of 0 is an empty primitive */
                gs_begin_variable(index);
            continue;
        }
        if (cached) {
            uint32_t slot = index & (VCACHE - 1);
            if (vcache[slot].gen == vcache_gen && vcache[slot].index == index) {
                assemble(&vcache[slot].v);
                continue;
            }
        }
        Vec4 attr[12];
        Vertex v;
        ShaderRegs s;
        load_attributes(index, attr);
        shade(attr, &s, gs ? NULL : &v);
        if (gs) {
            gs_feed(&s);
            continue;
        }
        if (cached) {
            uint32_t slot = index & (VCACHE - 1);
            vcache[slot].gen = vcache_gen;
            vcache[slot].index = index;
            vcache[slot].v = v;
        }
        assemble(&v);
    }
}

void pica_immediate_vertex(const Vec4 *attribs)
{
    if (gpu_skip_draws)
        return;
    check_unsupported();
    Vec4 attr[12];
    memcpy(attr, attribs, sizeof attr);
    Vertex v;
    ShaderRegs s;
    int gs = geometry_shader_on();
    if (gs && gs_mode == 1)
        gpu_fatal("immediate-mode vertices with a variable-size geometry shader");
    shade(attr, &s, gs ? NULL : &v);
    if (gs)
        gs_feed(&s);
    else
        assemble(&v);
}
