# Drawing on the GPU (Vulkan)

The software renderer in `gpu/` draws every pixel on the CPU: about 10-15 fps
in Pokémon X's towns. This plan moves the drawing onto the host GPU (the
owner's RTX 4060, through a native Windows build; later Metal on iOS through
MoltenVK). The software renderer stays as the reference: on the same command
lists, both must give the same picture (within rounding), and the tests check
it.

## What stays on the CPU

- Command lists (`gpu/regs.c`), the GX engines (`gpu/gx.c`), texture decoding
  (`gpu/texture.c`'s decoders).
- Vertex shading (`gpu/shader.c`, multithreaded in `gpu/draw.c`) and
  clipping. Triangles reach the GPU in window coordinates plus `1/w`, which
  is all a hardware rasteriser needs: position `(x_ndc*w, y_ndc*w, z*w, w)`
  gives perspective-correct interpolation for free.
- Vertex shaders on the GPU are a later, optional step.

## What moves to the GPU

1. **The pixel stage as one shader.** A single fragment shader reads the
   current state from a uniform block: the six combiner stages (sources,
   operands, modes, scales, constants, the buffer colour), the alpha test,
   fog, and fragment lighting. It is written once in GLSL, compiled at build
   time with `glslangValidator`, and embedded as SPIR-V, so no shader compiler
   ships. The branches are uniform across a draw, which a modern GPU handles
   well. If profiling says so, it can later be specialised per state.
2. **Fixed-function state from Vulkan:** depth test and write, stencil,
   blending, logic op, colour masks, face culling and scissor, mostly one to
   one with the PICA's. A small cache of `VkPipeline`s is keyed by the
   state that can't be dynamic.
3. **Textures:** decoded on the CPU with the existing code into RGBA8,
   uploaded once, and kept in a cache keyed by address, size and format,
   re-uploaded when the game rewrites that memory (hash, or the GX/CPU write
   that touched it).
4. **Render targets (the hard part).** Colour and depth buffers live in 3DS
   memory, tiled. A surface cache keeps a GPU image per buffer:
   - upload from 3DS memory when the game (CPU, MemoryFill, DisplayTransfer)
     wrote it since the GPU last did;
   - read back into 3DS memory (re-tiled) when something reads it: a
     DisplayTransfer or TextureCopy from it, a texture at its address
     (render to texture, like the mirrors in the intro), or the end of a
     frame. First version: read back at the end of every command list.
     Correct, and cheap at these sizes; then lazier.

## Steps

1. **Window and device** (done): `hle/window.c` presents the software frames
   through Vulkan on the 4060, with keyboard and mouse.
2. **First triangles** (done): `gpu/vkr.c` drains the same queue as the
   software rasteriser (`pica_raster_flush`), batch by batch, with
   `GPU_BACKEND=vulkan`. What it covers (`vkr_can_draw`; the rest falls back
   to software per batch): combiners on the vertex colour, constants, the
   previous stage and the buffer, in the shader with the software path's
   integer arithmetic; alpha test; depth (D32_SFLOAT holding the integer
   depth times 2^-bits, so tests compare the same integers), stencil,
   blending, logic ops, write masks, scissor. Each batch uploads its target
   from 3DS memory and reads it back, so GPU, software and CPU writes mix
   freely. Check: `make test-vulkan` draws every GPU example again on
   lavapipe; all 400-odd GPU batches give frames identical to software.
   Shaders: `gpu/shaders/`, compiled into the committed `gpu/vkr_shaders.h`
   by `make shaders`.
3. **Textures and combiners** (done): units 0-2 are decoded by texture.c's
   decoder into RGBA8 images (every reachable mip level, cube faces as
   layers), cached by address, size, format and a hash of their bytes. The
   shader reads texels with texelFetch and does texture.c's wrap, border,
   filtering, LOD and mip selection itself. Attributes reach the shader
   divided by w with 1/w beside them, and it divides as draw.c does; the
   neighbours for mipmapping come from the exact slopes of those affine
   values. Units that are off read zero, as in software. Check: textured_cube
   and cubemap within one step of rounding on 2-3 pixels. Pokémon X in
   Lumiose's first square: about half its batches on the GPU (the frame
   within 4/255 of software); the rest need fragment lighting. A batch that
   stays on software is counted by reason, printed at exit.
4. **Lighting and fog** (done): the shader does lighting.c's per-pixel
   lighting (up to 8 lights, the 24 lookup tables, spotlights, distance
   attenuation, Fresnel, normal maps) and draw.c's fog, reading the lighting
   registers and tables from a storage buffer. What lighting.c stops on
   (shadows, tangent maps, geometric factors, cos(phi)) stays on software.
   Read-back goes through host-cached memory (0.7 to 6.1 fps on its own).
   Check: every lit example within one step of rounding, apart from a pixel
   or two where a table lookup lands on the other side of an entry. Pokémon
   X in Lumiose: every batch on the GPU, the frame visually identical. Still
   slow (about 3 fps), as each batch copies its target in and out: step 5.
   Left on software: the procedural texture (citro2d) and gas.
5. **Surface cache** (done): render targets stay on the GPU. Batches are
   recorded into one command buffer (render passes stay open while the
   target doesn't change; vertices, registers and tables go into per-
   submission ring buffers, one descriptor set per batch) and submitted by
   `vkr_sync`: at the end of each command list, before a batch drawn in
   software, before a texture is read from memory the GPU drew on, and when
   a surface or texture still in use would be evicted. It reads back only
   the targets that were drawn on. A surface checks a hash of its memory
   before its first use after a sync, so writes by the CPU, GX or software
   are uploaded. Pokémon X in Lumiose: 34.7 fps (software: 9.7), the frame
   identical to step 4; 7142 batches in 360 submissions.
6. **Pokémon X at 60 fps:** profile. At 34.7 fps a second of Lumiose
   spends about 0.67 s shading vertices on the CPU, 0.73 s in the GPU path
   (hashing, uploads, read-backs, waiting) and 0.31 s in game code and the OS.
   Candidates: read back only at the end of a frame or when memory is read,
   check memory by watching writes instead of hashing, vertex shading on
   the GPU.

`GPU_BACKEND=software` keeps the current renderer, for tests and for
comparing pictures.
