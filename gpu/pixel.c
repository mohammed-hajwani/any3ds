/* Pixel formats, tiling and float24. */
#include <math.h>
#include <string.h>

#include "gpu.h"

int pixel_size(int format)
{
    switch (format) {
    case PIXEL_RGBA8:  return 4;
    case PIXEL_RGB8:   return 3;
    case PIXEL_RGB565:
    case PIXEL_RGB5A1:
    case PIXEL_RGBA4:  return 2;
    default:           return 0;
    }
}

/* Widening n-bit channels to 8 bits by repeating the top bits, so the
   maximum maps to 255 and 0 to 0. */
static uint8_t from5(uint32_t v) { return (uint8_t)(v << 3 | v >> 2); }
static uint8_t from6(uint32_t v) { return (uint8_t)(v << 2 | v >> 4); }
static uint8_t from4(uint32_t v) { return (uint8_t)(v * 17); }

Rgba color_decode(const uint8_t *p, int format)
{
    uint32_t v = (uint32_t)p[0] | (uint32_t)p[1] << 8;
    switch (format) {
    case PIXEL_RGBA8:  return (Rgba){ p[3], p[2], p[1], p[0] };
    case PIXEL_RGB8:   return (Rgba){ p[2], p[1], p[0], 255 };
    case PIXEL_RGB565: return (Rgba){ from5(v >> 11), from6(v >> 5 & 63), from5(v & 31), 255 };
    case PIXEL_RGB5A1: return (Rgba){ from5(v >> 11), from5(v >> 6 & 31), from5(v >> 1 & 31), (uint8_t)(v & 1 ? 255 : 0) };
    case PIXEL_RGBA4:  return (Rgba){ from4(v >> 12), from4(v >> 8 & 15), from4(v >> 4 & 15), from4(v & 15) };
    default:           gpu_fatal("pixel format %d", format);
    }
}

void color_encode(uint8_t *p, int format, Rgba c)
{
    uint32_t v;
    switch (format) {
    case PIXEL_RGBA8:
        p[0] = c.a; p[1] = c.b; p[2] = c.g; p[3] = c.r;
        return;
    case PIXEL_RGB8:
        p[0] = c.b; p[1] = c.g; p[2] = c.r;
        return;
    case PIXEL_RGB565: v = (uint32_t)(c.r >> 3) << 11 | (uint32_t)(c.g >> 2) << 5 | (uint32_t)(c.b >> 3); break;
    case PIXEL_RGB5A1: v = (uint32_t)(c.r >> 3) << 11 | (uint32_t)(c.g >> 3) << 6 | (uint32_t)(c.b >> 3) << 1 | (c.a >> 7); break;
    case PIXEL_RGBA4:  v = (uint32_t)(c.r >> 4) << 12 | (uint32_t)(c.g >> 4) << 8 | (uint32_t)(c.b >> 4) << 4 | (uint32_t)(c.a >> 4); break;
    default:           gpu_fatal("pixel format %d", format);
    }
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

uint32_t tile_index(uint32_t x, uint32_t y, uint32_t width)
{
    uint32_t tile = (y / 8) * (width / 8) + x / 8;
    uint32_t morton = (x & 1) | (y & 1) << 1 | (x & 2) << 1 | (y & 2) << 2 | (x & 4) << 2 | (y & 4) << 3;
    return tile * 64 + morton;
}

float f24_to_f32(uint32_t v)
{
    uint32_t sign = v >> 23 & 1, exponent = v >> 16 & 0x7F, mantissa = v & 0xFFFF, bits;
    if (exponent == 0)
        bits = sign << 31;                                  /* zero (no denormals) */
    else if (exponent == 0x7F)
        bits = sign << 31 | 0xFFu << 23 | mantissa << 7;    /* infinity / NaN */
    else
        bits = sign << 31 | (exponent - 63 + 127) << 23 | mantissa << 7;
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

uint32_t f32_to_f24(float f)
{
    uint32_t i;
    memcpy(&i, &f, 4);
    uint32_t sign = i >> 31, mantissa = (i & 0x7FFFFF) >> 7;
    int32_t exponent = (int32_t)(i >> 23 & 0xFF) - 127 + 63;
    if (exponent < 0)
        return sign << 23;                                  /* underflow: zero */
    if (exponent > 0x7F)
        return sign << 23 | 0x7Fu << 16;                    /* overflow: infinity */
    return sign << 23 | (uint32_t)exponent << 16 | mantissa;
}
