/* Texture units 0-2: decoding and sampling (texture.c). */
#ifndef GPU_TEXTURE_H
#define GPU_TEXTURE_H

#include <stdint.h>

#include "gpu.h"

typedef struct {
    uint32_t addr[6];               /* physical; [1..5] are the other cube faces */
    uint32_t width, height, format, param, lod, border;
    int type;                       /* 0 = 2D, 1 = cube map */
} TexUnit;

/* Read texture unit 0, 1 or 2's registers. */
void tex_unit_read(int unit, TexUnit *t);

/* Sample at texture coordinates (u, v) (for a cube map, the direction
   (u, v, w)), at level of detail `lod` (log2 of texels per pixel). */
Rgba tex_sample(const TexUnit *t, float u, float v, float w, float lod);

/* The texel at memory position (mx, my) of a level `width` texels wide. */
Rgba tex_decode(const uint8_t *level, uint32_t format, uint32_t width, uint32_t mx, uint32_t my);

/* One pixel of a 4x4 ETC1 block (block as a 64-bit little-endian word). */
Rgba etc1_pixel(uint64_t block, uint32_t x, uint32_t y);

uint32_t tex_level_size(uint32_t format, uint32_t width, uint32_t height);

#endif
