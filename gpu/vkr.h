/* The Vulkan renderer (gpu/vkr.c, docs/vulkan-renderer.md): draws batches
 * of gpu/draw.c's queued triangles on the host GPU. */
#ifndef GPU_VKR_H
#define GPU_VKR_H

#include <stdint.h>

/* One vertex, as the vertex shader takes it: position in normalised device
   coordinates (y = -1 at the target's first memory row), the PICA's window
   depth, 1/w, and gpu/draw.c's 24 attribute slots divided by w. */
#define VKR_SLOTS 24
typedef struct { float x, y, z, inv_w, a[VKR_SLOTS]; } VkrVertex;

/* The render target, in 3DS memory (tiled), as gpu/draw.c's Target has it. */
typedef struct {
    uint8_t *color, *depth;             /* NULL when the batch doesn't use them */
    uint32_t width, height;
    int color_fmt, depth_fmt;           /* pixel format (gpu.h); depth: 0 = 16, 2 = 24, 3 = 24 + 8 stencil */
    int color_write, depth_read, depth_write, stencil_write;
} VkrTarget;

int  vkr_enabled(void);                 /* GPU_BACKEND=vulkan (and a device came up) */
int  vkr_can_draw(void);                /* the current register state is one the GPU path covers */
void vkr_draw(const VkrTarget *t, const VkrVertex *v, uint32_t n);    /* n vertices: n / 3 triangles */
/* Submit the recorded batches and put what they drew into 3DS memory. Call
   it before anything else reads or writes render targets: the end of a
   command list, a batch drawn in software. */
void vkr_sync(void);

#endif
