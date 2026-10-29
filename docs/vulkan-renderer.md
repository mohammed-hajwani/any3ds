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
2. **First triangles:** a `gpu/vk.c` backend behind the same queue that the
   software rasteriser drains (`pica_raster_flush`). Untextured, colour only,
   depth test. Check: devkitPro's `simple_tri` and `both_screens` match the
   software frames.
3. **Textures and combiners:** the uber-shader, the texture cache. Check: the
   textured and citro2d examples.
4. **Fragment ops, lighting and fog** in the shader; stencil, blending and
   logic ops in Vulkan state. Check: all 49 working examples match.
5. **Pokémon X:** the surface cache's read-back points, render to texture,
   then profile. Target: a steady 60 fps.

`GPU_BACKEND=software` keeps the current renderer, for tests and for
comparing pictures.
