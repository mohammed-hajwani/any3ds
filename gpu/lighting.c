/* Fragment lighting (3dbrew: GPU/Fragment_Lighting and the lighting
 * registers 0x140-0x1D9).
 *
 * The vertex shader outputs a quaternion (the rotation from surface space
 * to eye space) and the view vector; the rasteriser interpolates them and
 * this computes, per pixel and per light:
 *   L  the light vector (positional: light position + view vector; else
 *      the light's direction), V the view vector, H = normalize(L + V),
 *      N the normal (the quaternion applied to (0, 0, 1), or to a bump
 *      map's normal)
 *   primary   += (diffuse * max(L.N, 0) + ambient) * spot * attenuation
 *   secondary += (specular0 * D0 + specular1 * reflection * D1) * spot * attenuation
 * and the global ambient is added to the primary colour. D0, D1, the
 * reflection tables (RR, RG, RB), the spotlight table and the distance
 * attenuation table are 256-entry look-up tables indexed by a selectable
 * dot product (N.H, V.H, N.V, L.N, -L.P). What isn't covered (shadows,
 * geometric factors, tangent maps, the cos(phi) input) stops by name. */
#include <inttypes.h>
#include <math.h>
#include <string.h>

#include "lighting.h"
#include "pica.h"

enum { LUT_D0 = 0, LUT_D1 = 1, LUT_FR = 3, LUT_RB = 4, LUT_RG = 5, LUT_RR = 6, LUT_SP = 8, LUT_DA = 16 };

static float f16(uint32_t v)            /* float1.5.10 */
{
    uint32_t sign = v >> 15 & 1, exponent = v >> 10 & 31, mantissa = v & 0x3FF;
    float f = exponent ? ldexpf(1.0f + (float)mantissa / 1024, (int)exponent - 15) : ldexpf((float)mantissa / 1024, -14);
    return sign ? -f : f;
}

static float f20(uint32_t v)            /* float1.7.12 */
{
    uint32_t sign = v >> 19 & 1, exponent = v >> 12 & 127, mantissa = v & 0xFFF;
    if (!exponent)
        return 0;
    float f = ldexpf(1.0f + (float)mantissa / 4096, (int)exponent - 63);
    return sign ? -f : f;
}

static float fixed13(uint32_t v)        /* fixed1.1.11 */
{
    return (float)((int32_t)(v << 19) >> 19) / 2048;
}

typedef struct { float x, y, z; } V3;

static V3 v3(float x, float y, float z) { return (V3){ x, y, z }; }
static float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 add(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static float length(V3 a) { return sqrtf(dot(a, a)); }
static V3 normalize(V3 a)
{
    float l = length(a);
    return l > 0 ? v3(a.x / l, a.y / l, a.z / l) : a;
}

/* Rotate v by the unit quaternion q = (x, y, z, w), w the scalar part. */
static V3 rotate(const float q[4], V3 v)
{
    V3 u = v3(q[0], q[1], q[2]);
    float s = q[3];
    V3 t = v3(2 * (u.y * v.z - u.z * v.y), 2 * (u.z * v.x - u.x * v.z), 2 * (u.x * v.y - u.y * v.x));
    V3 c = v3(u.y * t.z - u.z * t.y, u.z * t.x - u.x * t.z, u.x * t.y - u.y * t.x);
    return v3(v.x + s * t.x + c.x, v.y + s * t.y + c.y, v.z + s * t.z + c.z);
}

/* An entry: 12-bit value (0..1) + 12-bit signed difference to the next. */
static float lut_entry(const uint32_t *table, int i, float frac)
{
    uint32_t e = table[i & 0xFF];
    float value = (float)(e & 0xFFF) / 4096, diff = (float)((int32_t)(e << 8) >> 20) / 2048;
    return value + diff * frac;
}

/* Look a dot product up: signed tables cover [-1, 1] with indices -128..127;
   with the abs flag (stored as 0 = abs!) they cover [0, 1] with 0..255. */
static float lut(int table, int which, float x)
{
    uint32_t abs_off = pica.reg[0x1D0] >> (4 * which + 1) & 1;
    uint32_t scale_sel = pica.reg[0x1D2] >> (4 * which) & 7;
    static const float scales[8] = { 1, 2, 4, 8, 1, 1, 0.25f, 0.5f };
    float v;
    if (!abs_off) {
        x = fminf(fabsf(x), 1);
        float pos = x * 256;
        int i = pos >= 255 ? 255 : (int)pos;
        v = lut_entry(pica.lighting_lut[table], i, pos - (float)i);
    } else {
        x = fmaxf(fminf(x, 1), -1);
        float pos = x * 128;
        int i = (int)floorf(pos);
        if (i > 127)
            i = 127;
        v = lut_entry(pica.lighting_lut[table], i, pos - (float)i);
    }
    return v * scales[scale_sel];
}

static float color_channel(uint32_t reg, int ch)     /* 8 bits at 20 (red), 10, 0 (blue) */
{
    return (float)(reg >> (20 - 10 * ch) & 0xFF) / 255;
}

/* Which tables a light environment configuration provides. */
static int available(uint32_t config, int table)
{
    switch (config) {
    case 0: return table == LUT_D0 || table == LUT_RR || table == LUT_SP || table == LUT_DA;
    case 1: return table == LUT_FR || table == LUT_RR || table == LUT_SP || table == LUT_DA;
    case 2: return table == LUT_D0 || table == LUT_D1 || table == LUT_RR || table == LUT_DA;
    case 3: return table == LUT_D0 || table == LUT_D1 || table == LUT_FR || table == LUT_DA;
    case 4: return table != LUT_FR;
    case 5: return table != LUT_D1;
    case 6: return table != LUT_RB && table != LUT_RG;
    default: return 1;
    }
}

/* The selected input for table `which` (0 D0, 1 D1, 2 SP, 3 FR, 4 RB, 5 RG, 6 RR). */
static float input(int which, V3 n, V3 v, V3 l, V3 h, V3 spot)
{
    switch (pica.reg[0x1D1] >> (4 * which) & 7) {
    case 0:  return dot(n, h);
    case 1:  return dot(v, h);
    case 2:  return dot(n, v);
    case 3:  return dot(l, n);
    case 4:  return dot(l, spot);       /* -L.P: the stored spot direction is already negated */
    default: gpu_fatal("lighting table input cos(phi) is not implemented yet");
    }
}

void lighting_compute(const float quat[4], const float view[3], const Rgba *bump, Rgba *primary, Rgba *secondary)
{
    uint32_t c0 = pica.reg[0x1C3], c1 = pica.reg[0x1C4], env = c0 >> 4 & 0xF;
    if (c0 & 1)
        gpu_fatal("shadowed fragment lighting is not implemented yet");
    float q[4] = { quat[0], quat[1], quat[2], quat[3] };
    float ql = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (int i = 0; i < 4 && ql > 0; i++)
        q[i] /= ql;

    V3 surface = v3(0, 0, 1);
    uint32_t bump_mode = c0 >> 28 & 3;
    if (bump_mode == 1 && bump) {       /* a normal map: (0..1) -> (-1..1) */
        surface = v3((float)bump->r / 127.5f - 1, (float)bump->g / 127.5f - 1, (float)bump->b / 127.5f - 1);
        if (!(c0 >> 30 & 1))
            surface.z = sqrtf(fmaxf(0, 1 - surface.x * surface.x - surface.y * surface.y));
        surface = normalize(surface);
    } else if (bump_mode > 1) {
        gpu_fatal("tangent-map lighting is not implemented yet");
    }
    V3 n = normalize(rotate(q, surface));
    V3 view_v = v3(view[0], view[1], view[2]), v = normalize(view_v);

    float prim[3], sec[3];
    for (int ch = 0; ch < 3; ch++) {
        prim[ch] = color_channel(pica.reg[0x1C0], ch);      /* global ambient */
        sec[ch] = 0;
    }
    float fresnel = 1;
    int nlights = (int)(pica.reg[0x1C2] & 7) + 1;
    for (int k = 0; k < nlights; k++) {
        int id = (int)(pica.reg[0x1D9] >> (4 * k) & 7);
        const uint32_t *r = &pica.reg[0x140 + 0x10 * id];
        uint32_t cfg = r[9];
        if (cfg & 0xC)
            gpu_fatal("lighting geometric factors are not implemented yet");
        V3 pos = v3(f16(r[4] & 0xFFFF), f16(r[4] >> 16), f16(r[5] & 0xFFFF));
        V3 l_unnorm = (cfg & 1) ? pos : add(pos, view_v);
        V3 l = normalize(l_unnorm), h = normalize(add(l, v));
        V3 spot = v3(fixed13(r[6] & 0x1FFF), fixed13(r[6] >> 16 & 0x1FFF), fixed13(r[7] & 0x1FFF));

        float ln = dot(l, n);
        ln = (cfg & 2) ? fabsf(ln) : fmaxf(ln, 0);

        float att = 1;
        if (!(c1 >> (24 + id) & 1) && available(env, LUT_DA)) {
            float d = f20(r[10]) + f20(r[11]) * length(l_unnorm);
            d = fminf(fmaxf(d, 0), 1) * 256;
            int i = d >= 255 ? 255 : (int)d;
            att = lut_entry(pica.lighting_lut[LUT_DA + id], i, d - (float)i);
        }
        if (!(c1 >> (8 + id) & 1) && available(env, LUT_SP))
            att *= lut(LUT_SP + id, 2, input(2, n, v, l, h, spot));

        float d0 = !(c1 >> 16 & 1) && available(env, LUT_D0) ? lut(LUT_D0, 0, input(0, n, v, l, h, spot)) : 1;
        float d1 = !(c1 >> 17 & 1) && available(env, LUT_D1) ? lut(LUT_D1, 1, input(1, n, v, l, h, spot)) : 1;
        float refl[3];
        refl[0] = !(c1 >> 22 & 1) && available(env, LUT_RR) ? lut(LUT_RR, 6, input(6, n, v, l, h, spot)) : 1;
        refl[1] = !(c1 >> 21 & 1) && available(env, LUT_RG) ? lut(LUT_RG, 5, input(5, n, v, l, h, spot)) : refl[0];
        refl[2] = !(c1 >> 20 & 1) && available(env, LUT_RB) ? lut(LUT_RB, 4, input(4, n, v, l, h, spot)) : refl[0];
        if (!(c1 >> 19 & 1) && available(env, LUT_FR))
            fresnel = lut(LUT_FR, 3, input(3, n, v, l, h, spot));

        int highlight = !(c0 >> 27 & 1) || dot(l, n) > 0;          /* clamp highlights */
        for (int ch = 0; ch < 3; ch++) {
            float diffuse = color_channel(r[2], ch) * ln + color_channel(r[3], ch);
            float specular = color_channel(r[0], ch) * d0 + color_channel(r[1], ch) * refl[ch] * d1;
            prim[ch] += diffuse * att;
            if (highlight)
                sec[ch] += specular * att;
        }
    }
    uint32_t fsel = c0 >> 2 & 3;
    uint8_t fa = (uint8_t)lroundf(fminf(fmaxf(fresnel, 0), 1) * 255);
    *primary = (Rgba){ (uint8_t)lroundf(fminf(prim[0], 1) * 255), (uint8_t)lroundf(fminf(prim[1], 1) * 255),
                       (uint8_t)lroundf(fminf(prim[2], 1) * 255), (fsel & 1) ? fa : 255 };
    *secondary = (Rgba){ (uint8_t)lroundf(fminf(sec[0], 1) * 255), (uint8_t)lroundf(fminf(sec[1], 1) * 255),
                         (uint8_t)lroundf(fminf(sec[2], 1) * 255), (fsel & 2) ? fa : 255 };
}
