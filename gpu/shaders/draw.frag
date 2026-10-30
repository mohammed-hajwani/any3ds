// The pixel stage of the Vulkan renderer (gpu/vkr.c): the six colour
// combiners, the alpha test and the depth value, in the same integer
// arithmetic as gpu/draw.c's software version, so both give the same
// colours. Blending, logic ops, the depth and stencil tests and the write
// masks are Vulkan's fixed-function state.
//
// Only state that gpu/vkr.c's vkr_can_draw accepts reaches this shader:
// combiner sources are the vertex colour, the constant, the previous stage
// and the buffer (textures, lighting and fog stay on the software path).
#version 450

layout(location = 0) in vec4 v_color;
layout(location = 1) noperspective in float v_z;

layout(location = 0) out vec4 o_color;

// Raw PICA registers, packed by vkr.c (STATE_* there):
//   words 0-29   combiner stage i at 5 * i: source, operand, combiner, constant, scale
//   30           the combiner buffer's initial colour
//   31           TEXENV_UPDATE_BUFFER
//   32           ALPHA_TEST
//   33           depth bits (16 or 24)
layout(set = 0, binding = 0) uniform State { uvec4 u[16]; };

uint reg(int i) { return u[i >> 2][i & 3]; }

ivec4 unpack_color(uint v)
{
    return ivec4(v & 0xFFu, v >> 8 & 0xFFu, v >> 16 & 0xFFu, v >> 24);
}

ivec4 source(uint src, ivec4 primary, ivec4 prev, ivec4 buf, ivec4 k, int stage)
{
    if (src == 0u) return primary;
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
    // lroundf(clamp(c) * 255), as the software rasteriser rounds
    ivec4 primary = ivec4(floor(clamp(v_color, 0.0, 1.0) * 255.0 + 0.5));
    ivec4 prev = ivec4(0), buf = unpack_color(reg(30));
    uint update = reg(31);
    for (int i = 0; i < 6; i++) {
        uint src = reg(5 * i), op = reg(5 * i + 1), comb = reg(5 * i + 2), scale = reg(5 * i + 4);
        ivec4 k = unpack_color(reg(5 * i + 3));
        uint rgb_mode = comb & 0xFu, alpha_mode = comb >> 16 & 0xFu;
        ivec3 rgb[3];
        int alpha[3];
        for (int j = 0; j < 3; j++) {
            rgb[j] = rgb_operand(source(src >> (4 * j) & 0xFu, primary, prev, buf, k, i), op >> (4 * j) & 0xFu);
            alpha[j] = alpha_operand(source(src >> (16 + 4 * j) & 0xFu, primary, prev, buf, k, i), op >> (12 + 4 * j) & 7u);
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
    uint z = uint(clamp(v_z, 0.0, 1.0) * float((1u << bits) - 1u));
    gl_FragDepth = float(z) * exp2(-float(bits));
    o_color = vec4(prev) / 255.0;
}
