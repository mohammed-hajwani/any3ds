// The pixel stage of the Vulkan renderer (gpu/vkr.c): attribute
// interpolation, texture units 0-2, the six colour combiners, the alpha test
// and the depth value, in the same arithmetic as gpu/draw.c and
// gpu/texture.c, so both renderers give the same colours. Blending, logic
// ops, the depth and stencil tests and the write masks are Vulkan's
// fixed-function state.
//
// Only state that gpu/vkr.c's vkr_can_draw accepts reaches this shader:
// combiner sources are the vertex colour, textures 0-2 (or zero when a unit
// is off), fragment lighting (gpu/lighting.c), the constant, the previous
// stage and the buffer, then fog. The procedural texture and gas stay on the
// software path.
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
//   37           bit 0: fragment lighting is on and a combiner reads it
//   40 + 8 * i   texture unit i: width, height, param, lod, border, type, -, coordinate set
layout(set = 0, binding = 0) uniform State { uvec4 u[16]; };

// Texture units 0-2, decoded to RGBA8: rows in memory order, cube faces as
// layers, every mip level texture.c can read. Read with texelFetch only.
layout(set = 0, binding = 1) uniform sampler2DArray tex[3];

// The lighting and fog state, copied by vkr.c when either is on:
//   words 0-153      registers 0x140-0x1D9 (lights, lighting config, tables)
//   154              FOG_COLOR
//   256 + 256 * i    lighting table i (24 of them)
//   6400             the fog table (128 entries)
layout(std430, set = 0, binding = 2) readonly buffer Tables { uint tbl[]; };

uint reg(int i) { return u[i >> 2][i & 3]; }
uint lreg(int r) { return tbl[r - 0x140]; }

// gpu/draw.c's attribute slots (SEM_*)
const int SEM_QUAT = 4, SEM_VIEW = 18, SEM_COLOR = 8, SEM_TC0 = 12, SEM_TC1 = 14, SEM_TC0W = 16, SEM_TC2 = 22;

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

// ---------------------------------------------------------------- fragment lighting (lighting.c)

const int LUT_D0 = 0, LUT_D1 = 1, LUT_FR = 3, LUT_RB = 4, LUT_RG = 5, LUT_RR = 6, LUT_SP = 8, LUT_DA = 16;

float f16(uint v)                       // float1.5.10
{
    uint e = v >> 10 & 31u, m = v & 0x3FFu;
    float f = e != 0u ? ldexp(1.0 + float(m) / 1024.0, int(e) - 15) : ldexp(float(m) / 1024.0, -14);
    return (v >> 15 & 1u) != 0u ? -f : f;
}

float f20(uint v)                       // float1.7.12
{
    uint e = v >> 12 & 127u, m = v & 0xFFFu;
    if (e == 0u)
        return 0.0;
    float f = ldexp(1.0 + float(m) / 4096.0, int(e) - 63);
    return (v >> 19 & 1u) != 0u ? -f : f;
}

float fixed13(uint v) { return float(int(v << 19) >> 19) / 2048.0; }

vec3 norm(vec3 a)                       // lighting.c's normalize: zero stays zero
{
    float l = length(a);
    return l > 0.0 ? a / l : a;
}

vec3 rotate(vec4 q, vec3 v)             // by the unit quaternion q, w the scalar part
{
    vec3 t = 2.0 * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

// An entry: 12-bit value (0..1) + 12-bit signed difference to the next.
float lut_entry(int table, int i, float frac)
{
    uint e = tbl[256 + 256 * table + (i & 0xFF)];
    return float(e & 0xFFFu) / 4096.0 + float(int(e << 8) >> 20) / 2048.0 * frac;
}

float lut(int table, int which, float x)
{
    const float scales[8] = float[8](1.0, 2.0, 4.0, 8.0, 1.0, 1.0, 0.25, 0.5);
    bool abs_off = (lreg(0x1D0) >> (4 * which + 1) & 1u) != 0u;
    float v;
    if (!abs_off) {
        x = min(abs(x), 1.0);
        float pos = x * 256.0;
        int i = pos >= 255.0 ? 255 : int(pos);
        v = lut_entry(table, i, pos - float(i));
    } else {
        x = clamp(x, -1.0, 1.0);
        float pos = x * 128.0;
        int i = min(int(floor(pos)), 127);
        v = lut_entry(table, i, pos - float(i));
    }
    return v * scales[lreg(0x1D2) >> (4 * which) & 7u];
}

bool available(uint config, int table)
{
    switch (config) {
    case 0u: return table == LUT_D0 || table == LUT_RR || table == LUT_SP || table == LUT_DA;
    case 1u: return table == LUT_FR || table == LUT_RR || table == LUT_SP || table == LUT_DA;
    case 2u: return table == LUT_D0 || table == LUT_D1 || table == LUT_RR || table == LUT_DA;
    case 3u: return table == LUT_D0 || table == LUT_D1 || table == LUT_FR || table == LUT_DA;
    case 4u: return table != LUT_FR;
    case 5u: return table != LUT_D1;
    case 6u: return table != LUT_RB && table != LUT_RG;
    default: return true;
    }
}

// The selected input for table `which`; cos(phi) (5+) is refused by vkr.c.
float lut_input(int which, vec3 n, vec3 v, vec3 l, vec3 h, vec3 spot)
{
    switch (lreg(0x1D1) >> (4 * which) & 7u) {
    case 0u: return dot(n, h);
    case 1u: return dot(v, h);
    case 2u: return dot(n, v);
    case 3u: return dot(l, n);
    default: return dot(l, spot);
    }
}

float channel(uint r, int ch) { return float(r >> (20 - 10 * ch) & 0xFFu) / 255.0; }

// (uint8_t)lroundf(x * 255): half away from zero, negatives wrap
int to_byte(float x)
{
    float v = x * 255.0;
    return int(v >= 0.0 ? floor(v + 0.5) : -floor(-v + 0.5)) & 255;
}

void lighting(vec4 quat, vec3 view, bool bumped, ivec4 bump, out ivec4 primary, out ivec4 secondary)
{
    uint c0 = lreg(0x1C3), c1 = lreg(0x1C4), env = c0 >> 4 & 0xFu;
    float ql = length(quat);
    vec4 q = ql > 0.0 ? quat / ql : quat;

    vec3 surface = vec3(0.0, 0.0, 1.0);
    if (bumped) {                       // a normal map (vkr.c refuses tangent maps)
        surface = vec3(bump.rgb) / 127.5 - 1.0;
        if ((c0 >> 30 & 1u) == 0u)
            surface.z = sqrt(max(0.0, 1.0 - surface.x * surface.x - surface.y * surface.y));
        surface = norm(surface);
    }
    vec3 n = norm(rotate(q, surface)), v = norm(view);

    uint ambient = lreg(0x1C0);
    vec3 prim = vec3(channel(ambient, 0), channel(ambient, 1), channel(ambient, 2)), sec = vec3(0.0);
    float fresnel = 1.0;
    int nlights = int(lreg(0x1C2) & 7u) + 1;
    for (int k = 0; k < nlights; k++) {
        int id = int(lreg(0x1D9) >> (4 * k) & 7u), r = 0x140 + 0x10 * id;
        uint cfg = lreg(r + 9);
        vec3 pos = vec3(f16(lreg(r + 4) & 0xFFFFu), f16(lreg(r + 4) >> 16), f16(lreg(r + 5) & 0xFFFFu));
        vec3 l_unnorm = (cfg & 1u) != 0u ? pos : pos + view;
        vec3 l = norm(l_unnorm), h = norm(l + v);
        vec3 spot = vec3(fixed13(lreg(r + 6) & 0x1FFFu), fixed13(lreg(r + 6) >> 16 & 0x1FFFu),
                         fixed13(lreg(r + 7) & 0x1FFFu));

        float ln = dot(l, n);
        ln = (cfg & 2u) != 0u ? abs(ln) : max(ln, 0.0);

        float att = 1.0;
        if ((c1 >> (24 + id) & 1u) == 0u && available(env, LUT_DA)) {
            float d = clamp(f20(lreg(r + 10)) + f20(lreg(r + 11)) * length(l_unnorm), 0.0, 1.0) * 256.0;
            int i = d >= 255.0 ? 255 : int(d);
            att = lut_entry(LUT_DA + id, i, d - float(i));
        }
        if ((c1 >> (8 + id) & 1u) == 0u && available(env, LUT_SP))
            att *= lut(LUT_SP + id, 2, lut_input(2, n, v, l, h, spot));

        float d0 = (c1 >> 16 & 1u) == 0u && available(env, LUT_D0) ? lut(LUT_D0, 0, lut_input(0, n, v, l, h, spot)) : 1.0;
        float d1 = (c1 >> 17 & 1u) == 0u && available(env, LUT_D1) ? lut(LUT_D1, 1, lut_input(1, n, v, l, h, spot)) : 1.0;
        vec3 refl;
        refl.r = (c1 >> 22 & 1u) == 0u && available(env, LUT_RR) ? lut(LUT_RR, 6, lut_input(6, n, v, l, h, spot)) : 1.0;
        refl.g = (c1 >> 21 & 1u) == 0u && available(env, LUT_RG) ? lut(LUT_RG, 5, lut_input(5, n, v, l, h, spot)) : refl.r;
        refl.b = (c1 >> 20 & 1u) == 0u && available(env, LUT_RB) ? lut(LUT_RB, 4, lut_input(4, n, v, l, h, spot)) : refl.r;
        if ((c1 >> 19 & 1u) == 0u && available(env, LUT_FR))
            fresnel = lut(LUT_FR, 3, lut_input(3, n, v, l, h, spot));

        bool highlight = (c0 >> 27 & 1u) == 0u || dot(l, n) > 0.0;     // clamp highlights
        for (int ch = 0; ch < 3; ch++) {
            float diffuse = channel(lreg(r + 2), ch) * ln + channel(lreg(r + 3), ch);
            float specular = channel(lreg(r), ch) * d0 + channel(lreg(r + 1), ch) * refl[ch] * d1;
            prim[ch] += diffuse * att;
            if (highlight)
                sec[ch] += specular * att;
        }
    }
    uint fsel = c0 >> 2 & 3u;
    int fa = to_byte(clamp(fresnel, 0.0, 1.0));
    primary = ivec4(to_byte(min(prim.r, 1.0)), to_byte(min(prim.g, 1.0)), to_byte(min(prim.b, 1.0)),
                    (fsel & 1u) != 0u ? fa : 255);
    secondary = ivec4(to_byte(min(sec.r, 1.0)), to_byte(min(sec.g, 1.0)), to_byte(min(sec.b, 1.0)),
                      (fsel & 2u) != 0u ? fa : 255);
}

// ---------------------------------------------------------------- fog (draw.c's apply_fog)

ivec4 fog(ivec4 c, float depth)
{
    float d = clamp(depth, 0.0, 1.0);
    if ((reg(31) >> 16 & 1u) != 0u)
        d = 1.0 - d;
    float x = d * 128.0;
    int i = x >= 127.0 ? 127 : int(x);
    uint e = tbl[6400 + i];
    float k = clamp(float(e >> 13 & 0x7FFu) / 2048.0 + float(int(e << 19) >> 19) / 2048.0 * (x - float(i)), 0.0, 1.0);
    uint fc = tbl[154];
    vec3 f = vec3(fc & 0xFFu, fc >> 8 & 0xFFu, fc >> 16 & 0xFFu);
    c.rgb = ivec3(floor(vec3(c.rgb) * k + f * (1.0 - k) + 0.5));
    return c;
}

// ---------------------------------------------------------------- combiners (draw.c)

ivec4 source(uint src, ivec4 primary, ivec4 light[2], ivec4 texc[3], ivec4 prev, ivec4 buf, ivec4 k, int stage)
{
    if (src == 0u) return primary;
    if (src == 1u || src == 2u) return light[src - 1u];
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

    // Fragment lighting (word 37 bit 0: on and read; off, it reads word 36)
    ivec4 light[2] = ivec4[2](unpack_color(reg(36)), unpack_color(reg(36)));
    if ((reg(37) & 1u) != 0u) {
        uint c0 = lreg(0x1C3);
        bool bumped = (c0 >> 28 & 3u) != 0u;
        vec4 quat = vec4(slot(a, SEM_QUAT), slot(a, SEM_QUAT + 1), slot(a, SEM_QUAT + 2), slot(a, SEM_QUAT + 3));
        vec3 view = vec3(slot(a, SEM_VIEW), slot(a, SEM_VIEW + 1), slot(a, SEM_VIEW + 2));
        lighting(quat, view, bumped, bumped ? texc[c0 >> 22 & 3u] : ivec4(0), light[0], light[1]);
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
            rgb[j] = rgb_operand(source(src >> (4 * j) & 0xFu, primary, light, texc, prev, buf, k, i), op >> (4 * j) & 0xFu);
            alpha[j] = alpha_operand(source(src >> (16 + 4 * j) & 0xFu, primary, light, texc, prev, buf, k, i),
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

    if ((update & 7u) == 5u)
        prev = fog(prev, v_zw.x);

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
