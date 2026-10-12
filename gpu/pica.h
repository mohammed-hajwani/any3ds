/* The PICA200's internal state: the register file that command lists
 * write, plus the memories filled through "FIFO" registers (shader code,
 * operand descriptors, float uniforms, look-up tables). Register numbers
 * and meanings from 3dbrew's GPU/Internal_Registers. */
#ifndef GPU_PICA_H
#define GPU_PICA_H

#include <stdint.h>

#include "gpu.h"

#define PICA_REGS 0x400

/* Register ids (the ones the emulation acts on or reads). */
enum {
    REG_FINALIZE = 0x010,
    REG_FACECULLING = 0x040, REG_VIEWPORT_WIDTH = 0x041, REG_VIEWPORT_HEIGHT = 0x043,
    REG_FRAGOP_CLIP = 0x047, REG_DEPTHMAP_SCALE = 0x04D, REG_DEPTHMAP_OFFSET = 0x04E,
    REG_SH_OUTMAP_TOTAL = 0x04F, REG_SH_OUTMAP_O0 = 0x050,     /* O0..O6 */
    REG_EARLYDEPTH_CLEAR = 0x063, REG_SCISSOR_MODE = 0x065, REG_SCISSOR_POS = 0x066, REG_SCISSOR_DIM = 0x067,
    REG_VIEWPORT_XY = 0x068, REG_DEPTHMAP_ENABLE = 0x06D,
    REG_TEXUNIT_CONFIG = 0x080, REG_LIGHTING_ENABLE0 = 0x08F,
    REG_PROCTEX_LUT = 0x0AF, REG_PROCTEX_LUT_DATA0 = 0x0B0,    /* ..0x0B7 */
    REG_TEXENV_UPDATE_BUFFER = 0x0E0, REG_FOG_COLOR = 0x0E1,
    REG_FOG_LUT_INDEX = 0x0E6, REG_FOG_LUT_DATA0 = 0x0E8,      /* ..0x0EF */
    REG_TEXENV_BUFFER_COLOR = 0x0FD,
    REG_COLOR_OPERATION = 0x100, REG_BLEND_FUNC = 0x101, REG_LOGIC_OP = 0x102, REG_BLEND_COLOR = 0x103,
    REG_ALPHA_TEST = 0x104, REG_STENCIL_TEST = 0x105, REG_STENCIL_OP = 0x106, REG_DEPTH_COLOR_MASK = 0x107,
    REG_FRAMEBUFFER_INVALIDATE = 0x110, REG_FRAMEBUFFER_FLUSH = 0x111,
    REG_COLORBUFFER_READ = 0x112, REG_COLORBUFFER_WRITE = 0x113,
    REG_DEPTHBUFFER_READ = 0x114, REG_DEPTHBUFFER_WRITE = 0x115,
    REG_DEPTHBUFFER_FORMAT = 0x116, REG_COLORBUFFER_FORMAT = 0x117, REG_FRAMEBUFFER_BLOCK32 = 0x11B,
    REG_DEPTHBUFFER_LOC = 0x11C, REG_COLORBUFFER_LOC = 0x11D, REG_FRAMEBUFFER_DIM = 0x11E,
    REG_GAS_LUT_INDEX = 0x123, REG_GAS_LUT_DATA = 0x124,
    REG_LIGHTING_LUT_INDEX = 0x1C5, REG_LIGHTING_LUT_DATA0 = 0x1C8,    /* ..0x1CF */
    REG_ATTRIBBUFFERS_LOC = 0x200, REG_ATTRIBBUFFERS_FORMAT_LOW = 0x201, REG_ATTRIBBUFFERS_FORMAT_HIGH = 0x202,
    REG_ATTRIBBUFFER0_OFFSET = 0x203,                           /* 12 buffers x 3 registers */
    REG_INDEXBUFFER_CONFIG = 0x227, REG_NUMVERTICES = 0x228, REG_GEOSTAGE_CONFIG = 0x229,
    REG_VERTEX_OFFSET = 0x22A, REG_DRAWARRAYS = 0x22E, REG_DRAWELEMENTS = 0x22F, REG_VTX_FUNC = 0x231,
    REG_FIXEDATTRIB_INDEX = 0x232, REG_FIXEDATTRIB_DATA0 = 0x233,   /* ..0x235 */
    REG_CMDBUF_SIZE0 = 0x238, REG_CMDBUF_ADDR0 = 0x23A, REG_CMDBUF_JUMP0 = 0x23C,
    REG_VSH_NUM_ATTR = 0x242, REG_START_DRAW_FUNC0 = 0x245, REG_GEOSTAGE_CONFIG2 = 0x253,
    REG_PRIMITIVE_CONFIG = 0x25E, REG_RESTART_PRIMITIVE = 0x25F,
    REG_GSH_BASE = 0x280, REG_VSH_BASE = 0x2B0,                 /* the two shader units' register blocks */
};

/* Offsets inside a shader unit's block (GSH at 0x280, VSH at 0x2B0). */
enum {
    SH_BOOLUNIFORM = 0x00, SH_INTUNIFORM_I0 = 0x01, SH_INPUTBUFFER_CONFIG = 0x09, SH_ENTRYPOINT = 0x0A,
    SH_ATTRIBUTES_PERMUTATION_LOW = 0x0B, SH_ATTRIBUTES_PERMUTATION_HIGH = 0x0C, SH_OUTMAP_MASK = 0x0D,
    SH_CODETRANSFER_END = 0x0F, SH_FLOATUNIFORM_INDEX = 0x10, SH_FLOATUNIFORM_DATA = 0x11,   /* ..0x18 */
    SH_CODETRANSFER_INDEX = 0x1B, SH_CODETRANSFER_DATA = 0x1C,                              /* ..0x23 */
    SH_OPDESCS_INDEX = 0x25, SH_OPDESCS_DATA = 0x26,                                        /* ..0x2D */
};

typedef struct { float x, y, z, w; } Vec4;

/* One shader unit's memories (the vertex shader's, or the geometry shader's). */
typedef struct {
    uint32_t code[4096];            /* 4096 words of program memory */
    uint32_t opdesc[128];           /* operand descriptors */
    Vec4     uniform[96];           /* c0-c95 */
    uint32_t code_index, opdesc_index;
    uint32_t uniform_index, uniform_f32;
    uint32_t uniform_words[4];      /* a uniform being assembled from 3 (f24) or 4 (f32) words */
    uint32_t uniform_count;
} ShaderUnit;

typedef struct {
    uint32_t reg[PICA_REGS];
    ShaderUnit vsh, gsh;

    Vec4     fixed_attrib[12];      /* GPUREG_FIXEDATTRIB_*: attributes with one value for every vertex */
    uint32_t attrib_words[3];       /* fixed attribute / immediate vertex being assembled */
    uint32_t attrib_count;
    Vec4     immediate[16];         /* immediate mode: the vertex being assembled, one attribute at a time */
    uint32_t immediate_attrib;

    uint32_t lighting_lut[24][256];
    uint32_t fog_lut[128];
    uint32_t proctex_lut[8][256];   /* noise, RGB map, alpha map, colour, colour difference */
    uint32_t gas_lut[16];
} Pica;

extern Pica pica;

/* A shader unit's registers while it runs one vertex (shader.c). */
typedef struct ShaderRegs {
    Vec4 v[16], o[16], r[16];       /* inputs, outputs, temporaries */
    int32_t a0x, a0y, aL;           /* address registers */
    int cmpx, cmpy;                 /* condition flags set by CMP */
    /* Geometry shaders: SETEMIT picks the slot (0-2) of the next emitted
       vertex and whether it completes a primitive (and its winding); EMIT
       calls on_emit with the output registers. */
    int emit_vertex, emit_prim, emit_winding;
    void (*on_emit)(struct ShaderRegs *s);
} ShaderRegs;

/* Run a unit's program from word `entry` until END. bools: b0-b15;
   ints: i0-i3 as (x, y, z, w) bytes. */
void shader_exec(const ShaderUnit *unit, uint32_t entry, uint16_t bools, const uint8_t ints[4][4], ShaderRegs *s);
extern const char *const pica_reg_names[PICA_REGS];

/* Run a command list (virtual address, size in bytes). */
void pica_run_command_list(uint32_t addr, uint32_t size);

/* The register value as a float24 / float1.7.23 (bits 1-31) / signed field. */
float reg_f24(uint32_t id);

/* Draw: triggered by GPUREG_DRAWARRAYS / DRAWELEMENTS (draw.c). */
void pica_draw(int indexed);
void pica_immediate_vertex(const Vec4 *attribs);   /* one immediate-mode vertex is complete */
void pica_restart_primitive(void);

#endif
