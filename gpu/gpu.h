/* Our own emulation of the 3DS GPU (the PICA200) and its 2D engines, written
 * from public documentation (3dbrew, libctru / citro3d headers).
 *
 * The app drives the GPU through GSP: it queues GX commands in GSP shared
 * memory (fills, copies, "run this command list"), and command lists are
 * streams of GPU register writes. Everything here works on guest memory.
 *
 * Addresses: GX commands carry the app's virtual addresses. Inside command
 * lists, the GPU registers hold physical ones (the app converts them with
 * osConvertVirtToPhys); gpu_phys_to_virt maps them back. */
#ifndef GPU_GPU_H
#define GPU_GPU_H

#include <stdint.h>

void gpu_init(uint8_t *guest_mem);

/* Guest memory at a virtual address; stops the run if [addr, addr + size)
   isn't somewhere the GPU could reach. */
uint8_t *gpu_mem(uint32_t addr, uint32_t size);

/* Physical -> virtual for the GPU's address registers: FCRAM (0x20000000,
   the app's linear heap at 0x14000000) and VRAM (0x18000000, at
   0x1F000000). 0 if unmapped. */
uint32_t gpu_phys_to_virt(uint32_t paddr);

/* A hard stop naming what the GPU can't do yet. */
void gpu_fatal(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

extern int gpu_trace;       /* GPU_TRACE=1: log every GX command and register write */

/* ---- Pixels ---- */

/* The 2D engines' and framebuffers' pixel formats (GX_TRANSFER_FORMAT,
   GPU_COLORBUF). Multi-byte formats are little-endian words with red in
   the top bits, so RGBA8 is stored A, B, G, R and RGB8 is B, G, R. */
enum { PIXEL_RGBA8 = 0, PIXEL_RGB8 = 1, PIXEL_RGB565 = 2, PIXEL_RGB5A1 = 3, PIXEL_RGBA4 = 4 };

typedef struct { uint8_t r, g, b, a; } Rgba;

int  pixel_size(int format);                /* bytes per pixel; 0 if unknown */
Rgba color_decode(const uint8_t *p, int format);
void color_encode(uint8_t *p, int format, Rgba c);

/* GPU images are tiled: 8x8-pixel tiles, row by row, and inside a tile
   the 64 pixels go in Morton (Z) order: x and y bits interleaved, x first.
   Returns the pixel's index in a tiled image `width` pixels wide. */
uint32_t tile_index(uint32_t x, uint32_t y, uint32_t width);

/* ---- float24: the GPU's own float (1 sign, 7 exponent bits biased by 63,
   16 mantissa bits). No denormals: a zero exponent reads as zero. ---- */
float    f24_to_f32(uint32_t v);
uint32_t f32_to_f24(float f);               /* truncates, as libctru's f32tof24 does */

/* Run a PICA command list: a stream of GPU register writes (regs.c). */
void pica_run_command_list(uint32_t addr, uint32_t size);

/* ---- GX commands (the 2D engines) ---- */

/* Fill [start, end) with `value`; control bits 8-9 pick 16/24/32-bit
   units, bit 0 means "do it". start == 0: unused. */
void gx_memory_fill(uint32_t start, uint32_t value, uint32_t end, uint32_t control);

/* Copy an image, converting format, tiled <-> linear and optionally
   downscaling 2x1 or 2x2 (anti-aliasing). Dimensions: height << 16 | width. */
void gx_display_transfer(uint32_t in, uint32_t out, uint32_t in_dim, uint32_t out_dim, uint32_t flags);

/* A raw copy of `size` bytes in lines: each dimension word is
   gap << 16 | line width, both in 16-byte units. */
void gx_texture_copy(uint32_t in, uint32_t out, uint32_t size, uint32_t in_dim, uint32_t out_dim, uint32_t flags);

#endif
