/* The GX commands: the GPU's 2D engines (memory fill, display transfer,
 * texture copy). */
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu.h"

static uint8_t *mem;
int gpu_trace;
int gpu_skip_draws;
static uint32_t trace_from;     /* GPU_TRACE=N (N > 1): start tracing at frame N */
static uint32_t skip_until;     /* GPU_SKIP_UNTIL=N: draw nothing before frame N */

void gpu_init(uint8_t *guest_mem)
{
    mem = guest_mem;
    const char *t = getenv("GPU_TRACE");
    trace_from = t ? (uint32_t)strtoul(t, NULL, 0) : 0;
    gpu_trace = t && trace_from <= 1;
    const char *s = getenv("GPU_SKIP_UNTIL");
    skip_until = s ? (uint32_t)strtoul(s, NULL, 0) : 0;
    gpu_skip_draws = skip_until > 0;
}

void gpu_frame(uint32_t frame)
{
    if (trace_from > 1 && frame == trace_from) {
        gpu_trace = 1;
        fprintf(stderr, "gpu: tracing from frame %" PRIu32 "\n", frame);
    }
    if (gpu_skip_draws && frame >= skip_until) {
        gpu_skip_draws = 0;
        fprintf(stderr, "gpu: drawing from frame %" PRIu32 "\n", frame);
    }
    gpu_stats_report(frame);
}

void gpu_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "gpu: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(3);
}

/* The memory the GPU can see: the linear heap (FCRAM) and VRAM. */
static int reachable(uint32_t addr, uint32_t size)
{
    uint64_t end = (uint64_t)addr + size;
    return (addr >= 0x14000000u && end <= 0x1C000000u) ||   /* linear heap (old mapping) */
           (addr >= 0x30000000u && end <= 0x40000000u) ||   /* linear heap (new mapping) */
           (addr >= 0x1F000000u && end <= 0x1F600000u);     /* VRAM */
}

uint8_t *gpu_mem(uint32_t addr, uint32_t size)
{
    if (!reachable(addr, size))
        gpu_fatal("access to %08" PRIx32 "..%08" PRIx32 ", outside the memory the GPU can reach",
                  addr, addr + size);
    return mem + addr;
}

uint32_t gpu_phys_to_virt(uint32_t paddr)
{
    if (paddr >= 0x20000000u && paddr < 0x28000000u)
        return paddr - 0x20000000u + 0x14000000u;
    if (paddr >= 0x18000000u && paddr < 0x18600000u)
        return paddr - 0x18000000u + 0x1F000000u;
    return 0;
}

void gx_memory_fill(uint32_t start, uint32_t value, uint32_t end, uint32_t control)
{
    if (!start || !(control & 1))
        return;
    uint32_t unit = (control >> 8 & 3) == 0 ? 2 : (control >> 8 & 3) == 1 ? 3 : 4;
    if (end < start)
        gpu_fatal("MemoryFill from %08" PRIx32 " to %08" PRIx32, start, end);
    uint32_t size = end - start;
    uint8_t *p = gpu_mem(start, size);
    uint8_t bytes[4] = { (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24) };
    /* One unit, then copy what is filled onto the rest, doubling each time
       (a byte loop with `i % unit` was a fifth of a whole game's run time). */
    uint32_t done = unit < size ? unit : size;
    memcpy(p, bytes, done);
    while (done < size) {
        uint32_t n = done < size - done ? done : size - done;
        memcpy(p + done, p, n);
        done += n;
    }
    if (gpu_trace)
        fprintf(stderr, "    GX MemoryFill %08" PRIx32 "..%08" PRIx32 " with %08" PRIx32 " (%" PRIu32 "-bit)\n",
                start, end, value, unit * 8);
}

void gx_display_transfer(uint32_t in, uint32_t out, uint32_t in_dim, uint32_t out_dim, uint32_t flags)
{
    uint32_t in_w = in_dim & 0xFFFF, in_h = in_dim >> 16, out_w = out_dim & 0xFFFF, out_h = out_dim >> 16;
    int flip = flags & 1, to_tiled = flags >> 1 & 1, in_fmt = flags >> 8 & 7, out_fmt = flags >> 12 & 7;
    uint32_t scale = flags >> 24 & 3;
    if (flags & 8)
        gpu_fatal("DisplayTransfer raw-copy mode (flags %08" PRIx32 ")", flags);
    if (scale == 3 || !pixel_size(in_fmt) || !pixel_size(out_fmt))
        gpu_fatal("DisplayTransfer flags %08" PRIx32, flags);
    uint32_t sx = scale ? 2 : 1, sy = scale == 2 ? 2 : 1;      /* input pixels per output pixel */
    if (out_w * sx > in_w || out_h * sy > in_h || in_w % 8 || in_h % 8)
        gpu_fatal("DisplayTransfer %" PRIu32 "x%" PRIu32 " -> %" PRIu32 "x%" PRIu32 " (flags %08" PRIx32 ")",
                  in_w, in_h, out_w, out_h, flags);
    int in_bpp = pixel_size(in_fmt), out_bpp = pixel_size(out_fmt);
    uint8_t *src = gpu_mem(in, in_w * in_h * (uint32_t)in_bpp);
    uint8_t *dst = gpu_mem(out, out_w * out_h * (uint32_t)out_bpp);

    for (uint32_t oy = 0; oy < out_h; oy++)
        for (uint32_t ox = 0; ox < out_w; ox++) {
            uint32_t sum[4] = { 0 };
            for (uint32_t dy = 0; dy < sy; dy++)
                for (uint32_t dx = 0; dx < sx; dx++) {
                    uint32_t ix = ox * sx + dx, iy = oy * sy + dy;
                    /* The tiled side is addressed in tiles, the linear one in rows. */
                    uint32_t i = to_tiled ? iy * in_w + ix : tile_index(ix, iy, in_w);
                    Rgba c = color_decode(src + i * (uint32_t)in_bpp, in_fmt);
                    sum[0] += c.r; sum[1] += c.g; sum[2] += c.b; sum[3] += c.a;
                }
            uint32_t n = sx * sy, ty = flip ? out_h - 1 - oy : oy;
            Rgba c = { (uint8_t)(sum[0] / n), (uint8_t)(sum[1] / n), (uint8_t)(sum[2] / n), (uint8_t)(sum[3] / n) };
            uint32_t o = to_tiled ? tile_index(ox, ty, out_w) : ty * out_w + ox;
            color_encode(dst + o * (uint32_t)out_bpp, out_fmt, c);
        }
    if (gpu_trace)
        fprintf(stderr, "    GX DisplayTransfer %08" PRIx32 " %" PRIu32 "x%" PRIu32 " -> %08" PRIx32 " %" PRIu32 "x%" PRIu32
                " (flags %08" PRIx32 ")\n", in, in_w, in_h, out, out_w, out_h, flags);
}

void gx_texture_copy(uint32_t in, uint32_t out, uint32_t size, uint32_t in_dim, uint32_t out_dim, uint32_t flags)
{
    uint32_t in_line = (in_dim & 0xFFFF) * 16, in_gap = (in_dim >> 16) * 16;
    uint32_t out_line = (out_dim & 0xFFFF) * 16, out_gap = (out_dim >> 16) * 16;
    if (!in_line || !out_line)
        gpu_fatal("TextureCopy with a zero line width (dims %08" PRIx32 " %08" PRIx32 ")", in_dim, out_dim);
    (void)flags;
    uint32_t si = 0, so = 0, in_used = 0, out_used = 0;
    for (uint32_t done = 0; done < size; done++) {
        *gpu_mem(out + so, 1) = *gpu_mem(in + si, 1);
        si++;
        so++;
        if (++in_used == in_line) {
            si += in_gap;
            in_used = 0;
        }
        if (++out_used == out_line) {
            so += out_gap;
            out_used = 0;
        }
    }
    if (gpu_trace)
        fprintf(stderr, "    GX TextureCopy %08" PRIx32 " -> %08" PRIx32 ", %" PRIu32 " bytes\n", in, out, size);
}
