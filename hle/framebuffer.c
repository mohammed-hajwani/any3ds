/* Turning a 3DS framebuffer into an ordinary image.
 *
 * 3DS gotcha: the LCDs are mounted sideways, so framebuffers are stored
 * rotated. Memory holds 240-pixel columns, one after another from the
 * left edge of the screen, and each column runs from the bottom of the
 * screen to the top. Pixel (x, y) of the upright 400x240 top screen is at
 * index x * 240 + (239 - y). Pixel formats are the GPU's: BGR8 is 3 bytes
 * B, G, R; RGB565 is a 16-bit word with red in the top bits; and so on. */
#include <stdio.h>
#include <string.h>

#include "service.h"

/* GSPGPU_FramebufferFormat */
enum { FMT_RGBA8, FMT_BGR8, FMT_RGB565, FMT_RGB5A1, FMT_RGBA4 };

static uint8_t expand(uint32_t v, int bits)
{
    return (uint8_t)(v * 255 / ((1u << bits) - 1));
}

static int bytes_per_pixel(uint32_t format)
{
    switch (format) {
    case FMT_RGBA8: return 4;
    case FMT_BGR8:  return 3;
    default:        return 2;
    }
}

int fb_to_rgb(const uint8_t *fb, uint32_t format, uint32_t stride, int width, uint8_t *rgb)
{
    if (format > FMT_RGBA4)
        return 0;
    int bpp = bytes_per_pixel(format);
    for (int x = 0; x < width; x++)
        for (int y = 0; y < 240; y++) {
            const uint8_t *p = fb + (size_t)x * stride + (size_t)(239 - y) * (size_t)bpp;
            uint8_t *out = rgb + ((size_t)y * (size_t)width + (size_t)x) * 3;
            uint32_t v = (uint32_t)p[0] | (uint32_t)p[1] << 8;
            switch (format) {
            case FMT_RGBA8:  out[0] = p[3]; out[1] = p[2]; out[2] = p[1]; break;
            case FMT_BGR8:   out[0] = p[2]; out[1] = p[1]; out[2] = p[0]; break;
            case FMT_RGB565: out[0] = expand(v >> 11, 5); out[1] = expand(v >> 5 & 63, 6); out[2] = expand(v & 31, 5); break;
            case FMT_RGB5A1: out[0] = expand(v >> 11, 5); out[1] = expand(v >> 6 & 31, 5); out[2] = expand(v >> 1 & 31, 5); break;
            case FMT_RGBA4:  out[0] = expand(v >> 12, 4); out[1] = expand(v >> 8 & 15, 4); out[2] = expand(v >> 4 & 15, 4); break;
            }
        }
    return 1;
}

static void put16(FILE *f, uint32_t v) { fputc((int)(v & 0xFF), f); fputc((int)(v >> 8 & 0xFF), f); }
static void put32(FILE *f, uint32_t v) { put16(f, v & 0xFFFF); put16(f, v >> 16); }

/* A 24-bit .bmp: no dependencies, and Windows opens it natively. */
int bmp_write(const char *path, const uint8_t *rgb, int width, int height)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return 0;
    uint32_t row = ((uint32_t)width * 3 + 3) & ~3u, size = row * (uint32_t)height;
    fputc('B', f);
    fputc('M', f);
    put32(f, 54 + size);
    put32(f, 0);
    put32(f, 54);                   /* pixel data offset */
    put32(f, 40);                   /* BITMAPINFOHEADER */
    put32(f, (uint32_t)width);
    put32(f, (uint32_t)height);     /* positive: rows stored bottom-up */
    put16(f, 1);
    put16(f, 24);
    put32(f, 0);
    put32(f, size);
    put32(f, 2835);                 /* 72 dpi */
    put32(f, 2835);
    put32(f, 0);
    put32(f, 0);
    for (int y = height - 1; y >= 0; y--) {
        for (int x = 0; x < width; x++) {
            const uint8_t *p = rgb + ((size_t)y * (size_t)width + (size_t)x) * 3;
            fputc(p[2], f);
            fputc(p[1], f);
            fputc(p[0], f);
        }
        for (uint32_t pad = (uint32_t)width * 3; pad < row; pad++)
            fputc(0, f);
    }
    return fclose(f) == 0;
}
