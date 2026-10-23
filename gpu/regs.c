/* Command lists: streams of register writes.
 *
 * Each command is a parameter word followed by a header word:
 *   bits 0-15   register id
 *   bits 16-19  byte mask: which bytes of the parameter are written
 *   bits 20-27  number of extra parameter words that follow
 *   bit 31      consecutive mode: extra parameters go to id+1, id+2, ...
 *               (otherwise they all go to the same register, a FIFO)
 * and commands are padded to a multiple of 8 bytes. Most registers just
 * hold configuration; a few trigger work (draw) or feed a memory (shader
 * code, uniforms, look-up tables) one word at a time. */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "pica.h"

Pica pica;

float reg_f24(uint32_t id)
{
    return f24_to_f32(pica.reg[id] & 0xFFFFFF);
}

/* x, y, z, w are four float24s packed into 96 bits (x lowest), sent as
   three words, highest first (how libctru and citro3d send uniforms and
   fixed attributes). */
static Vec4 unpack_f24x4(const uint32_t w[3])
{
    uint32_t lo = w[2], mid = w[1], hi = w[0];
    return (Vec4){
        f24_to_f32(lo & 0xFFFFFF),
        f24_to_f32((lo >> 24 | mid << 8) & 0xFFFFFF),
        f24_to_f32((mid >> 16 | hi << 16) & 0xFFFFFF),
        f24_to_f32(hi >> 8),
    };
}

static float f32_of(uint32_t bits)
{
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static void shader_unit_write(ShaderUnit *u, uint32_t off, uint32_t value)
{
    if (off == SH_FLOATUNIFORM_INDEX) {
        u->uniform_index = value & 0xFF;
        u->uniform_f32 = value >> 31;
        u->uniform_count = 0;
    } else if (off >= SH_FLOATUNIFORM_DATA && off < SH_FLOATUNIFORM_DATA + 8) {
        u->uniform_words[u->uniform_count++] = value;
        uint32_t need = u->uniform_f32 ? 4 : 3;
        if (u->uniform_count == need) {
            Vec4 v;
            if (u->uniform_f32)     /* w, z, y, x */
                v = (Vec4){ f32_of(u->uniform_words[3]), f32_of(u->uniform_words[2]),
                            f32_of(u->uniform_words[1]), f32_of(u->uniform_words[0]) };
            else
                v = unpack_f24x4(u->uniform_words);
            if (u->uniform_index < 96)
                u->uniform[u->uniform_index] = v;
            u->uniform_index++;
            u->uniform_count = 0;
        }
    } else if (off == SH_CODETRANSFER_INDEX) {
        u->code_index = value & 0xFFF;
    } else if (off >= SH_CODETRANSFER_DATA && off < SH_CODETRANSFER_DATA + 8) {
        u->code[u->code_index++ & 0xFFF] = value;
    } else if (off == SH_OPDESCS_INDEX) {
        u->opdesc_index = value & 0x7F;
    } else if (off >= SH_OPDESCS_DATA && off < SH_OPDESCS_DATA + 8) {
        u->opdesc[u->opdesc_index++ & 0x7F] = value;
    }
}

static void fixed_attrib_word(uint32_t value)
{
    pica.attrib_words[pica.attrib_count++] = value;
    if (pica.attrib_count < 3)
        return;
    pica.attrib_count = 0;
    Vec4 v = unpack_f24x4(pica.attrib_words);
    uint32_t index = pica.reg[REG_FIXEDATTRIB_INDEX] & 0xF;
    if (index == 0xF) {         /* immediate mode: this is the next attribute of the vertex being sent */
        pica.immediate[pica.immediate_attrib++] = v;
        if (pica.immediate_attrib == (pica.reg[REG_VSH_NUM_ATTR] & 0xF) + 1) {
            pica.immediate_attrib = 0;
            pica_immediate_vertex(pica.immediate);
        }
    } else if (index < 12) {
        pica.fixed_attrib[index] = v;
    } else {
        gpu_fatal("fixed attribute index %" PRIu32, index);
    }
}

/* What a register write asks the command list loop to do. */
enum { NEXT, FINALIZE, JUMP0, JUMP1 };

/* Write a register (bits outside `mask` keep their value) and do what the
   write triggers. */
static int reg_write(uint32_t id, uint32_t value, uint32_t mask)
{
    if (id >= PICA_REGS)
        gpu_fatal("command list writes register %03" PRIx32 ", which doesn't exist", id);
    /* Queued triangles must see the state they were drawn with. Registers
       from 0x200 up (vertex arrays, primitive setup, the shader units) only
       shape triangles not yet queued, so writing them needn't draw the
       queue: draws then batch together, which spreads them over threads. */
    if (id < 0x200)
        pica_raster_flush();
    pica.reg[id] = (pica.reg[id] & ~mask) | (value & mask);
    value = pica.reg[id];
    if (gpu_trace) {
        const char *name = pica_reg_names[id];
        fprintf(stderr, "      PICA %03" PRIx32 " %-32s = %08" PRIx32 "%s\n", id, name ? name : "?", value,
                mask == 0xFFFFFFFFu ? "" : " (masked)");
    }

    if (id >= REG_VSH_BASE && id < REG_VSH_BASE + 0x30) {
        shader_unit_write(&pica.vsh, id - REG_VSH_BASE, value);
        return NEXT;
    }
    if (id >= REG_GSH_BASE && id < REG_GSH_BASE + 0x30) {
        shader_unit_write(&pica.gsh, id - REG_GSH_BASE, value);
        return NEXT;
    }
    if (id >= REG_LIGHTING_LUT_DATA0 && id < REG_LIGHTING_LUT_DATA0 + 8) {
        uint32_t index = pica.reg[REG_LIGHTING_LUT_INDEX], table = index >> 8 & 0x1F, entry = index & 0xFF;
        if (table < 24)
            pica.lighting_lut[table][entry] = value;
        pica.reg[REG_LIGHTING_LUT_INDEX] = (index & ~0xFFu) | ((entry + 1) & 0xFF);
        return NEXT;
    }
    if (id >= REG_FOG_LUT_DATA0 && id < REG_FOG_LUT_DATA0 + 8) {
        uint32_t index = pica.reg[REG_FOG_LUT_INDEX] & 0x7F;
        pica.fog_lut[index] = value;
        pica.reg[REG_FOG_LUT_INDEX] = (index + 1) & 0x7F;
        return NEXT;
    }
    if (id >= REG_PROCTEX_LUT_DATA0 && id < REG_PROCTEX_LUT_DATA0 + 8) {
        uint32_t index = pica.reg[REG_PROCTEX_LUT], table = index >> 8 & 7, entry = index & 0xFF;
        pica.proctex_lut[table][entry] = value;
        pica.reg[REG_PROCTEX_LUT] = (index & ~0xFFu) | ((entry + 1) & 0xFF);
        return NEXT;
    }

    switch (id) {
    case REG_FINALIZE:
        return FINALIZE;
    case REG_CMDBUF_JUMP0:
    case REG_CMDBUF_JUMP0 + 1:
        return value ? JUMP0 + (int)(id - REG_CMDBUF_JUMP0) : NEXT;
    case REG_DRAWARRAYS:
    case REG_DRAWELEMENTS:
        if (value & 1)
            pica_draw(id == REG_DRAWELEMENTS);
        return NEXT;
    case REG_FIXEDATTRIB_INDEX:
        pica.attrib_count = 0;
        pica.immediate_attrib = 0;
        return NEXT;
    case REG_FIXEDATTRIB_DATA0:
    case REG_FIXEDATTRIB_DATA0 + 1:
    case REG_FIXEDATTRIB_DATA0 + 2:
        fixed_attrib_word(value);
        return NEXT;
    case REG_RESTART_PRIMITIVE:
        if (value & 1)
            pica_restart_primitive();
        return NEXT;
    case REG_GAS_LUT_DATA: {
        uint32_t index = pica.reg[REG_GAS_LUT_INDEX] & 0xF;
        pica.gas_lut[index] = value;
        pica.reg[REG_GAS_LUT_INDEX] = (index + 1) & 0xF;
        return NEXT;
    }
    default:
        /* Configuration, read at draw time; and triggers with nothing to do
           here (cache flush/invalidate, post-vertex cache clear). */
        return NEXT;
    }
}

static uint32_t byte_mask(uint32_t bits)
{
    uint32_t mask = 0;
    for (int i = 0; i < 4; i++)
        if (bits >> i & 1)
            mask |= 0xFFu << (8 * i);
    return mask;
}

static void run_list(uint32_t addr, uint32_t size)
{
    if (gpu_trace)
        fprintf(stderr, "    GX ProcessCommandList %08" PRIx32 ", %" PRIu32 " bytes\n", addr, size);
    size &= ~7u;                    /* the hardware ignores the low 3 bits of the size */
    const uint8_t *buf = gpu_mem(addr, size);
    uint32_t n = size / 4, i = 0;
    while (i + 1 < n) {
        uint32_t param, header;
        memcpy(&param, buf + 4 * i, 4);
        memcpy(&header, buf + 4 * i + 4, 4);
        uint32_t id = header & 0xFFFF, mask = byte_mask(header >> 16 & 0xF), extra = header >> 20 & 0xFF;
        int consecutive = header >> 31;
        if (i + 2 + extra > n)
            gpu_fatal("command list at %08" PRIx32 ": a command at word %" PRIu32 " runs past the end", addr, i);
        int action = reg_write(id, param, mask);
        for (uint32_t k = 0; k < extra && action == NEXT; k++) {
            uint32_t word;
            memcpy(&word, buf + 4 * (i + 2 + k), 4);
            action = reg_write(consecutive ? id + 1 + k : id, word, mask);
        }
        if (action == FINALIZE)
            return;
        if (action == JUMP0 || action == JUMP1) {
            /* Run the other command buffer instead of the rest of this one. */
            int b = action - JUMP0;
            uint32_t target = gpu_phys_to_virt(pica.reg[REG_CMDBUF_ADDR0 + b] << 3);
            uint32_t bytes = (pica.reg[REG_CMDBUF_SIZE0 + b] & 0x1FFFFF) << 3;
            if (!target)
                gpu_fatal("command buffer jump to physical %08" PRIx32, pica.reg[REG_CMDBUF_ADDR0 + b] << 3);
            run_list(target, bytes);
            return;
        }
        i += 2 + extra;
        i += i & 1;                 /* commands are padded to 8 bytes */
    }
}

void pica_run_command_list(uint32_t addr, uint32_t size)
{
    run_list(addr, size);
    pica_raster_flush();                /* the list's last triangles */
}
