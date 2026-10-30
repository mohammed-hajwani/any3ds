// The pixel stage of the Vulkan renderer (gpu/vkr.c): attribute
// interpolation, texture units 0-2, the six colour combiners, the alpha test
// and the depth value, in the same arithmetic as gpu/draw.c and
// gpu/texture.c, so both renderers give the same colours. Blending, logic
// ops, the depth and stencil tests and the write masks are Vulkan's
// fixed-function state.
//
// Only state that gpu/vkr.c's vkr_can_draw accepts reaches this shader:
// combiner sources are the vertex colour, textures 0-2 (or zero when a unit
// is off), lighting only while it is off, the constant, the
// previous stage and the buffer (lighting, procedural textures and fog stay
// on the software path).
#version 450

layout(location = 0) noperspective in vec4 v_zw;
layout(location = 1) noperspective in vec4 v_attr[6];

layout(location = 0) out vec4 o_color;

// Raw PICA registers, packed by vkr.c (vkr_draw):
//   words 0-29   combiner stage i at 5 * i: source, operand, combiner, constant, scale
//   30           the combiner buffer's initial colour
//   31           TEXENV_UPDATE_BUFFER
//   32           ALPHA_TEST
//   33           depth bits (16 or 24)
//   34           bit i: texture unit i is read
//   35           bit i: texture unit i is off or has no size, and reads word 36
//   36           what unset units and lighting that is off read (draw.c's unset_color)
//   40 + 8 * i   texture unit i: width, height, param, lod, border, type, -, coordinate set
layout(set = 0, binding = 0) uniform State { uvec4 u[16]; };

// Texture units 0-2, decoded to RGBA8: rows in memory order, cube faces as
// layers, every mip level texture.c can read. Read with texelFetch only.
layout(set = 0, binding = 1) uniform sampler2DArray tex[3];

uint reg(int i) { return u[i >> 2][i & 3]; }

// gpu/draw.c's attribute slots (SEM_*)
const int SEM_COLOR = 8, SEM_TC0 = 12, SEM_TC1 = 14, SEM_TC0W = 16, SEM_TC2 = 22;

float slot(vec4 a[6], int k) { return a[k >> 2][k & 3]; }

ivec4 unpack_color(uint v)
{
    return ivec4(v & 0xFFu, v >> 8 & 0xFFu, v >> 16 & 0xFFu, v >> 24);
}

// ---------------------------------------------------------------- textures (texture.c)

struct Unit { uint w, h, param, lod, border, type, set; };

Unit unit(int i)
{
    int b = 40 + 8 * i;
    return Unit(reg(b), reg(b + 1), reg(b + 2), reg(b + 3), reg(b + 4), reg(b + 5), reg(b + 7));
}

// c modulo n in [0, n), for negative c too (GLSL's % isn't defined there)
int modulo(int c, int n)
{
    return c >= 0 ? c % n : n - 1 - (-c - 1) % n;
}

int wrap(int c, int size, uint mode, inout bool border)
{
    switch (mode) {
    case 0u: return clamp(c, 0, size - 1);                             // clamp to edge
    case 1u: if (c < 0 || c >= size) border = true; return c;          // clamp to border
    case 2u: return modulo(c, size);                                   // repeat
    default: {                                                         // mirrored repeat
        int m = modulo(c, 2 * size);
        return m < size ? m : 2 * size - 1 - m;
    }
    }
}

vec4 texel(int i, ivec3 p, int level)
{
    vec4 c;
    if (i == 0) c = texelFetch(tex[0], p, level);
    else if (i == 1) c = texelFetch(tex[1], p, level);
    else c = texelFetch(tex[2], p, level);
    return floor(c * 255.0 + 0.5);
}

vec4 fetch(int i, Unit t, int level, int lw, int lh, int x, int y, int face)
{
    bool border = false;
    int wx = wrap(x, lw, t.param >> 12 & 7u, border), wy = wrap(y, lh, t.param >> 8 & 7u, border);
    if (border)
        return vec4(unpack_color(t.border));
    return texel(i, ivec3(wx, lh - 1 - wy, face), level);
}

vec4 sample_level(int i, Unit t, int level, uint lw, uint lh, float s, float tc, bool linear, int face)
{
    precise float x = s * float(lw), y = tc * float(lh);
    if (!linear)
        return fetch(i, t, level, int(lw), int(lh), int(floor(x)), int(floor(y)), face);
    x -= 0.5;
    y -= 0.5;
    int x0 = int(floor(x)), y0 = int(floor(y));
    precise float fx = x - float(x0), fy = y - float(y0);
    vec4 c00 = fetch(i, t, level, int(lw), int(lh), x0, y0, face);
    vec4 c10 = fetch(i, t, level, int(lw), int(lh), x0 + 1, y0, face);
    vec4 c01 = fetch(i, t, level, int(lw), int(lh), x0, y0 + 1, face);
    vec4 c11 = fetch(i, t, level, int(lw), int(lh), x0 + 1, y0 + 1, face);
    precise vec4 top = c00 + (c10 - c00) * fx, bottom = c01 + (c11 - c01) * fx;
    precise vec4 r = top + (bottom - top) * fy;
    return floor(r + 0.5);
}

// The cube face from the major axis of (u, v, w), and 2D coordinates on it.
int cube_face(float u, float v, float w, out float s, out float t)
{
    float au = abs(u), av = abs(v), aw = abs(w), sc, tc, ma;
    int face;
    if (au >= av && au >= aw) {
        face = u >= 0.0 ? 0 : 1;
        ma = au;
        sc = u >= 0.0 ? -w : w;
        tc = -v;
    } else if (av >= aw) {
        face = v >= 0.0 ? 2 : 3;
        ma = av;
        sc = u;
        tc = v >= 0.0 ? w : -w;
    } else {
        face = w >= 0.0 ? 4 : 5;
        ma = aw;
        sc = w >= 0.0 ? u : -u;
        tc = -v;
    }
    s = (sc / ma + 1.0) * 0.5;
    t = (tc / ma + 1.0) * 0.5;
    return face;
}

vec4 sample_unit(int i, Unit t, float u, float v, float w, float lod)
{
    int face = 0;
    if (t.type == 1u)
        face = cube_face(u, v, w, u, v);
    else if (t.type == 3u && w != 0.0) {
        u /= w;
        v /= w;
    }
    float bias = float(int(t.lod << 19) >> 19) / 256.0;
    uint max_level = t.lod >> 16 & 15u, min_level = t.lod >> 24 & 15u;
    lod += bias;
    bool minify = lod > 0.0;
    bool linear = ((minify ? t.param >> 2 : t.param >> 1) & 1u) != 0u;
    float clamped = max(float(min_level), min(float(max_level), lod));
    bool mip_linear = (t.param >> 24 & 1u) != 0u;
    uint level = minify ? uint(mip_linear ? floor(clamped) : floor(clamped + 0.5)) : min_level;
    if (level > max_level)
        level = max_level;
    uint lw = t.w, lh = t.h, l = 0u;
    for (; l < level && lw > 8u && lh > 8u; l++) {
        lw /= 2u;
        lh /= 2u;
    }
    vec4 c = sample_level(i, t, int(l), lw, lh, u, v, linear, face);
    if (minify && mip_linear && level < max_level && lw > 8u && lh > 8u && clamped > float(level)) {
        float f = clamped - float(level);
        vec4 d = sample_level(i, t, int(l) + 1, lw / 2u, lh / 2u, u, v, linear, face);
        precise vec4 r = c + (d - c) * f;
        c = floor(r + 0.5);
    }
    return c;
}

// ---------------------------------------------------------------- combiners (draw.c)

ivec4 source(uint src, ivec4 primary, ivec4 texc[3], ivec4 prev, ivec4 buf, ivec4 k, int stage)
{
    if (src == 0u) return primary;
    if (src == 1u || src == 2u) return unpack_color(reg(36));     // lighting is off (vkr_can_draw)
    if (src >= 3u && src <= 5u) return texc[src - 3u];
    if (src == 13u) return buf;
    if (src == 14u) return k;
    if (src == 15u && stage > 0) return prev;
    return ivec4(0);
}

ivec3 rgb_operand(ivec4 c, uint op)
{
    ivec3 v;
    switch (op >> 1) {
    case 0u: v = c.rgb; break;
    case 1u: v = ivec3(c.a); break;
    case 2u: v = ivec3(c.r); break;
    case 4u: v = ivec3(c.g); break;
    default: v = ivec3(c.b); break;     // 6; vkr_can_draw rejects the rest
    }
    return (op & 1u) != 0u ? 255 - v : v;
}

int alpha_operand(ivec4 c, uint op)
{
    int v;
    switch (op >> 1 & 3u) {
    case 0u: v = c.a; break;
    case 1u: v = c.r; break;
    case 2u: v = c.g; break;
    default: v = c.b; break;
    }
    return (op & 1u) != 0u ? 255 - v : v;
}

ivec3 combine(uint mode, ivec3 a, ivec3 b, ivec3 c)
{
    switch (mode) {
    case 0u: return a;
    case 1u: return a * b / 255;
    case 2u: return clamp(a + b, 0, 255);
    case 3u: return clamp(a + b - 128, 0, 255);
    case 4u: return (a * c + b * (255 - c)) / 255;
    case 5u: return clamp(a - b, 0, 255);
    case 8u: return clamp(a * b / 255 + c, 0, 255);
    default: return clamp(a + b, 0, 255) * c / 255;     // 9; vkr_can_draw rejects the rest
    }
}

bool test(uint func, uint a, uint b)
{
    switch (func) {
    case 0u: return false;
    case 1u: return true;
    case 2u: return a == b;
    case 3u: return a != b;
    case 4u: return a < b;
    case 5u: return a <= b;
    case 6u: return a > b;
    default: return a >= b;
    }
}

void main()
{
    // Attributes: (a/w) / (1/w), as draw.c's interpolate(). The neighbours
    // one pixel right and one up (window y grows upwards, the framebuffer's
    // rows downwards) for texture derivatives: a/w and 1/w are affine in
    // screen space, so value + slope is their value there.
    float iw = v_zw.y;
    vec4 a[6], ax[6], ay[6];
    float iwx = iw + dFdxFine(iw), iwy = iw - dFdyFine(iw);
    for (int k = 0; k < 6; k++) {
        a[k] = v_attr[k] / iw;
        ax[k] = (v_attr[k] + dFdxFine(v_attr[k])) / iwx;
        ay[k] = (v_attr[k] - dFdyFine(v_attr[k])) / iwy;
    }

    // lroundf(clamp(c) * 255), as the software rasteriser rounds
    vec4 col = vec4(slot(a, SEM_COLOR), slot(a, SEM_COLOR + 1), slot(a, SEM_COLOR + 2), slot(a, SEM_COLOR + 3));
    ivec4 primary = ivec4(floor(clamp(col, 0.0, 1.0) * 255.0 + 0.5));

    ivec4 texc[3] = ivec4[3](ivec4(0), ivec4(0), ivec4(0));
    uint used = reg(34);
    for (int i = 0; i < 3; i++) {
        if ((used >> i & 1u) == 0u)
            continue;
        if ((reg(35) >> i & 1u) != 0u) {
            texc[i] = unpack_color(reg(36));
            continue;
        }
        Unit t = unit(i);
        int s = i == 0 ? SEM_TC0 : i == 1 || t.set == 1u ? SEM_TC1 : SEM_TC2;
        float u0 = slot(a, s), v0 = slot(a, s + 1);
        vec2 dx = vec2(slot(ax, s) - u0, slot(ax, s + 1) - v0);
        vec2 dy = vec2(slot(ay, s) - u0, slot(ay, s + 1) - v0);
        float sx = length(dx * vec2(t.w, t.h)), sy = length(dy * vec2(t.w, t.h));
        float scale = max(sx, sy), lod = scale > 0.0 ? log2(scale) : -100.0;
        texc[i] = ivec4(sample_unit(i, t, u0, v0, slot(a, SEM_TC0W), lod));
    }

    ivec4 prev = ivec4(0), buf = unpack_color(reg(30));
    uint update = reg(31);
    for (int i = 0; i < 6; i++) {
        uint src = reg(5 * i), op = reg(5 * i + 1), comb = reg(5 * i + 2), scale = reg(5 * i + 4);
        ivec4 k = unpack_color(reg(5 * i + 3));
        uint rgb_mode = comb & 0xFu, alpha_mode = comb >> 16 & 0xFu;
        ivec3 rgb[3];
        int alpha[3];
        for (int j = 0; j < 3; j++) {
            rgb[j] = rgb_operand(source(src >> (4 * j) & 0xFu, primary, texc, prev, buf, k, i), op >> (4 * j) & 0xFu);
            alpha[j] = alpha_operand(source(src >> (16 + 4 * j) & 0xFu, primary, texc, prev, buf, k, i),
                                     op >> (12 + 4 * j) & 7u);
        }
        ivec4 o;
        if (rgb_mode == 6u || rgb_mode == 7u) {     // dot3: 4 * sum((a - 0.5)(b - 0.5))
            ivec3 p = (rgb[0] - 128) * (rgb[1] - 128);
            int d = (p.r + p.g + p.b) * 4;
            d = clamp(d < 0 ? -(-d / 255) : d / 255, 0, 255);  // C's division truncates towards zero
            o = ivec4(d, d, d, rgb_mode == 7u ? d : combine(alpha_mode, ivec3(alpha[0]), ivec3(alpha[1]), ivec3(alpha[2])).x);
        } else {
            o = ivec4(combine(rgb_mode, rgb[0], rgb[1], rgb[2]),
                      combine(alpha_mode, ivec3(alpha[0]), ivec3(alpha[1]), ivec3(alpha[2])).x);
        }
        o.rgb = clamp(o.rgb * (1 << (scale & 3u)), 0, 255);
        o.a = clamp(o.a * (1 << (scale >> 16 & 3u)), 0, 255);
        if (i < 4) {
            if ((update >> (8 + i) & 1u) != 0u) buf.rgb = o.rgb;
            if ((update >> (12 + i) & 1u) != 0u) buf.a = o.a;
        }
        prev = o;
    }

    uint at = reg(32);
    if ((at & 1u) != 0u && !test(at >> 4 & 7u, uint(prev.a), at >> 8 & 0xFFu))
        discard;

    // The depth buffer holds the PICA's integer depth scaled by 2^-bits, so
    // values and comparisons are exactly the software rasteriser's.
    uint bits = reg(33);
    uint z = uint(clamp(v_zw.x, 0.0, 1.0) * float((1u << bits) - 1u));
    gl_FragDepth = float(z) * exp2(-float(bits));
    o_color = vec4(prev) / 255.0;
}
