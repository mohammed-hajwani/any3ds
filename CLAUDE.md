# 3DS → iOS static recompiler

## What this project is

A personal side project: a **static recompiler** that translates Nintendo 3DS game code (32-bit ARMv6K, ARM + Thumb) ahead of time into **C source**, which is then compiled into a native arm64 iOS app.

Why: iOS forbids JIT, so 3DS emulators on iPhone fall back to slow interpreters. Translating the whole program ahead of time sidesteps that and runs at native speed.

The pipeline:

1. **Load**: read the program (homebrew ELF for now; later `code.bin` + CROs from a user's own dump).
2. **Lift**: disassemble ARM/Thumb instructions (Capstone) and recover functions and control flow.
3. **Emit**: write each guest function as a C function that operates on a CPU context struct.
4. **Compile**: clang builds the C. During development the target is x86-64 Linux (WSL2); iOS comes much later.
5. **Runtime**: OS calls (`SVC`) are routed to high-level emulation of the 3DS OS. The long-term plan is to reuse Azahar's HLE services, kernel and GPU code, and replace only its CPU backend with our precompiled code.
6. **Fallback**: indirect jumps that can't be resolved statically go through a lookup table (guest address → host function); misses drop into an interpreter.

We are **not** writing a full emulator. We are writing a CPU translator plus glue.

## Hard rules (legal)

- Never commit game code, ROMs, dumps, decryption keys, firmware or recompiled output of commercial games. Keep all of that in `dumps/` and `out/`, which are gitignored.
- Never download, search for or link to ROMs or keys.
- Compatibility lists, per-game configs (addresses, patch points) and dump hashes are fine. They're metadata, not game code.
- If any Azahar code is reused, the project must be GPLv2.

## Environment

- Windows + **WSL2 Ubuntu**. All commands run inside WSL. No Mac available.
- **devkitPro** (devkitARM + libctru) builds the 3DS test programs: `arm-none-eabi-gcc`.
- **Python 3** for the recompiler prototype: `capstone`, `pyelftools`.
- **clang** compiles the generated C on the host.
- iOS builds come much later (Theos on Linux + SideStore sideloading). Ignore them for now.

## Repo layout (planned)

```
recomp/            Python recompiler
  elf_loader.py    read ELF sections and symbols
  disasm.py        Capstone wrapper (ARM mode first, Thumb later)
  translate.py     instruction → C statement(s)
  emit_c.py        writes functions + a dispatch table to a .c file
  __main__.py      CLI: python -m recomp <input.elf> -o out/gen.c
runtime/
  context.h        CPU context struct + memory helpers
hle/               the OS layer: kernel (threads, sync, IPC) + services (srv, APT, GSP, HID, FS)
gpu/               our own PICA200 GPU: GX engines, command lists, shader interpreter, rasteriser
tests/
  compat/          the compatibility sweep over devkitPro's 3ds-examples (make compat)
  m0/              milestone 0 test program + harness
dumps/             (gitignored) user's own dumps
out/               (gitignored) generated code
```

## Core conventions for generated code

```c
typedef struct {
    uint32_t r[16];        // r0–r15 (r13 = sp, r14 = lr, r15 = pc)
    uint32_t n, z, c, v;   // condition flags, kept as 0/1
    uint8_t *mem;          // base of the 4 GiB guest address space
    union { uint32_t u[32]; uint64_t x[16]; float f[32]; double d[16]; } vfp;  // s0–s31 / d0–d15 (M5)
    uint32_t fpscr;        // VFP status; only NZCV is modelled
} Ctx;

static inline uint32_t read32(Ctx *c, uint32_t a)  { uint32_t v; memcpy(&v, c->mem + a, 4); return v; }
static inline void     write32(Ctx *c, uint32_t a, uint32_t v) { memcpy(c->mem + a, &v, 4); }
```

- Each guest function becomes `void f_XXXXXXXX(Ctx *ctx)`, named by guest address in hex.
- Guest memory is `ctx->mem + (uint32_t)addr`. Both guest and host are little-endian.
- `bx lr` at the end of a function becomes `return;`.
- Each instruction emits a comment with its address and original disassembly, so generated code is debuggable.
- An unsupported instruction is a **hard error** at recompile time, naming the address and mnemonic. Never silently skip anything.
- Conditional execution (almost every ARM instruction can be conditional) becomes `if (cond_XX(ctx)) { ... }`. Implement it from milestone 1.

## Status

- **M0 done.** `make test-m0`.
- **M1 done.** `make test-m1`. Tests are linked ELFs at `0x00100000` (unlinked `.o` files with relocations are rejected), and harnesses diff recompiled code against the same C compiled natively. push/pop and guest memory (`runtime/runtime.c`, 4 GiB `mmap`) came forward from M2, because every non-leaf function saves `lr` on the stack.
- **M2 done.** `make test-m2`. Covers loads and stores in every addressing mode, ldm/stm (ia/ib/da/db), literal pools (folded to constants using the `$a`/`$d` mapping symbols), gcc's `add pc, pc, rX, lsl #2` switch tables, and the program image loaded into guest memory (`recomp_load_image`) with a data-symbol table. Capstone gotcha: its `post_index` flag is always False for ldrh/strh/ldrsb/ldrsh, so we read the P bit (bit 24) from the encoding instead.
- **M3 done.** `make test-m3`. `python -m recomp --trace` emits `RECOMP_TRACE(addr, "disasm")` before every instruction, and write helpers report writes. `tests/m3/diff.c` runs every function of every linked test program in lockstep with Unicorn 2.0.1 (`libunicorn-dev`, CPU model ARM11MPCore, flat 4 GiB map), comparing r0–r14, NZCV and memory writes before every instruction. The first divergence is reported with the instruction that caused it. `tests/m3/selftest.sh` checks that a planted bug gets pinpointed. M3 found two real bugs: Capstone's `update_flags` is wrong for adc/sbc/rsc (we read the S bit, bit 20, instead), and accesses at the top of the address space didn't wrap (they read past the host mapping).
- **M4 done.** `make test-m4`. Thumb (ARMv6K = Thumb-1: 16-bit instructions plus the bl/blx pair; anything Thumb-2 is rejected) and ARM/Thumb interworking. The mode comes from symbol bit 0 plus the `$t`/`$a` mapping symbols. In Thumb, pc reads +4, and literal loads and adr use Align(pc, 4). Thumb flags come from the 16-bit encoding format. Thumb `bl` sets lr bit 0, `blx #imm` is a mode-switching C call, and Thumb switch tables (`mov pc, rX`) become a C switch over the function's instruction addresses. Also new: register-specified shifts (runtime `shift_*` helpers) and mul/mla. The M1/M2 programs are rebuilt as Thumb, and every M4 program also runs in lockstep against Unicorn. Known gaps: linker interworking veneers (`bx pc` + ARM `b`; the test links with `--use-blx`), tail calls, indirect calls/jumps, long multiplies.
- **M5 done.** `make test-m5`. VFPv2 floating point, in ARM mode only (Thumb-1 has no VFP), using the hard-float ABI (`-mfloat-abi=hard -mfpu=vfp`). `Ctx` gained a `vfp` union (`u[32]`/`x[16]` raw bits, `f`/`d` value views) and `fpscr` (NZCV only). All arithmetic goes through `runtime/vfp.h`, which encodes ARM's rules: NaN propagation (SNaN first, then operand order), the default NaN 0x7FC00000 (x86 gives 0xFFC00000), non-fused multiply-accumulate, and saturating float→int conversion. Literal-pool `vldr` is folded to constants with the value shown. The M3 driver now enables the VFP in Unicorn and compares d0–d15 and the FPSCR flags. Not modelled (these are hard errors): FPSCR rounding modes (`vcvtr`), reading the whole FPSCR, VFPv3 `vmov #imm`, flush-to-zero, default-NaN mode, exception flags.
- **Real-code readiness done.** `make test-real`. A realistic -O2 program linked with libgcc, built as ARM and Thumb, diffed against native and Unicorn. What it added:
  - **Function discovery:** call and branch targets without a symbol, such as libgcc's alternate entry points, become synthetic functions (`name+0xoff`) covering target to end, so that code is recompiled twice.
  - **Size-0 function symbols** from hand-written asm get their size inferred from the next symbol.
  - **Aliases** (two names at one address) are merged.
  - **Tail calls** (`b` to another function) become `f(ctx); return;`.
  - **Indirect calls** (`blx rX`, `bx rX`) go through `recomp_call` and a binary-searched `recomp_find` table. An unknown target aborts loudly.
  - **New instructions:** `clz`, `umull`/`smull`/`umlal`/`smlal`, `ldrd`/`strd`, `rev`/`rev16`/`revsh`, `sxtb`/`sxth`/`uxth`, and `push {sp, ...}`.
  - **Diff driver:** accepts a skip list for functions that take function pointers.
- **M6a done (recompiles real libctru).** `make test-m6` builds the canonical libctru hello world (`tests/m6/hello.c`) against real libctru + newlib, recompiles all 303 functions into ~47k lines of C that compile cleanly, and diffs newlib's hand-optimised strcmp/memcpy/memset/memmove/strlen/strchr against Unicorn. What real code added:
  - **Instructions:** `mrc` TLS reads (`Ctx.tls`), barriers as no-ops, `ldrex`/`strex`/`clrex` (a monitor in `Ctx`), `svc` → `recomp_svc` (an OS-layer hook, currently aborting with the svc number), `uadd8`/`sel` (`Ctx.ge`), `rsc`, `udf`, `push {…pc}`, and `ldrd` over its own base.
  - **noreturn calls:** a call with nothing after it gets `recomp_no_return`.
  - **Loader fixes:** functions are clipped at the next symbol, and unlabelled code in `$a`/`$t` regions becomes functions.
  - **VFP RunFast mode:** FPSCR FZ+DN, which libctru sets on every thread, via `vmsr fpscr`. Half of all Unicorn runs use it.
- **M6b done: the recompiled hello world runs and draws "Hello from recompiled code!".** `make test-m6` runs it end to end: START is pressed at frame 10, it exits via svcExitProcess, and the last frame (both screens) must match `tests/m6/hello_frame.sha256`. `make run-m6` saves that frame to `out/m6/frame.bmp`. The OS layer is `hle/`, our own HLE written from the 3dbrew docs (no Azahar code, so no GPL yet):
  - **Kernel (`hle/kernel.c`, `svc.c`):**
    - Threads: each guest thread gets its own host pthread (its own host stack for the recompiled C calls), but only one runs at a time. Switches happen only inside svcs: on a block, a yield, or when a higher-priority thread is woken. Runs are fully deterministic.
    - Virtual clock: each svc costs 1 µs, and when every thread is blocked the clock jumps to the next deadline (sleep, timeout, or the 60 Hz vblank).
    - Objects: events (oneshot/sticky/pulse), mutexes, semaphores, threads, address arbiters (libctru's LightLock/LightEvent), shared memory, and service sessions.
    - Calls: WaitSynchronization1/N (any/all, timeouts), and QueryMemory over a map of what is in use.
    - Failure reporting: no thread can run and nothing is scheduled → a deadlock report naming each thread and what it waits on (exit 4). A virtual-time limit catches hangs.
  - **Services (`hle/srv.c`, `apt.c`, `gsp.c`, `hid.c`, `fs.c`):**
    - srv: hands out sessions.
    - APT: Enable queues the WAKEUP parameter that HOME menu would send.
    - GSP: shared-memory interrupt queue, VBlank0/1 each frame, framebuffer latching.
    - HID: shared-memory pad state each frame, from a `--hold FRAME:KEYS` script.
    - FS: Initialize, and "no SD card".
    - An unknown IPC command is a hard stop (`HLE_LENIENT=1` replies success instead, to explore). `HLE_TRACE=1` logs every svc.
  - **Tests:**
    - `tests/m6/threads.c` (kernel only, empty `__appInit`) checks locks, events, semaphores, sleeps, timeouts, priorities and wait-any/all against a golden `threads.expected`.
    - `tests/m6/deadlock.c` checks the deadlock report.
  - **Translator fixes found by running real code:**
    - `uxtab`/`uxtah`/`sxtab`/`sxtah` (tested in `tests/real/extend.S`).
    - Tail calls now honour a redirected lr: crt0 does `ldr lr, =exit; bx r3` to run main and then exit. After a tail call, `recomp_continue` carries on at the lr the callee returned to, instead of returning straight to our caller.
  - **Next:**
    1. Real file access (FS + RomFS/SD in `dumps/`).
    2. GPU command lists (GX queue, then PICA200), which is where reusing Azahar starts to pay.
    3. Audio (DSP).
    4. Bigger homebrew as test cases.
- **M7 done: compatibility sweep over devkitPro's 3ds-examples.** The 62 public-domain examples installed with `3ds-dev` (`/opt/devkitpro/examples/3ds`) are the test corpus.
  - **The sweep:** `make compat` (`tests/compat/sweep.py`) copies each one into `out/compat/`, builds it, recompiles it, runs it on the HLE with scripted input, classifies the outcome and writes `docs/compat.md`. Expected outcomes are in `tests/compat/examples.txt`, last-frame hashes in `tests/compat/hashes.txt`; after looking at the frames, `--record` updates both. `make test` runs a quick subset of 4.
  - **Result:** 31 run to a clean exit with the right screens. The other 31:
    - 20 stop at the GPU: citro3d/citro2d's first GX command is MemoryFill, then ProcessCommandList, which needs a PICA200.
    - 3 need the system font, which is firmware data we never ship.
    - 8 don't build: missing portlibs or ImageMagick, a missing video file, a library template, and one example that is out of date with libctru.
    - 0 fail to recompile or compile.
  - **Translator fixes the corpus found** (each tested against Unicorn in `tests/real/dsp.S` or `tests/m5/vfp.S`):
    - Instructions: smulxy/smlaxy/smulwy/smlawy, usat/ssat, and fldmx/fstmx (the pre-UAL VFP block forms, which move one extra word).
    - Fall-through into the next function (libgcc's `__aeabi_dsub` runs on into `__adddf3`): a tail call.
    - Old-style control flow: `mov pc, lr`, and any `ldm` that loads pc (APCS `ldmdb fp, {fp, sp, pc}`).
    - **Bug:** jump tables with *negative* indices (newlib's `_vfprintf_r` loads signed offsets with ldrsh) jumped to the default.
    - Coprocessors the 3DS doesn't have (iWMMXt) and VFP d16-d31 now compile to `recomp_undefined`: a trap, exactly as the ARM11 would raise an Undefined Instruction.
    - Labels are emitted as `L_x: ;` (a label can end a block).
  - **HLE additions:**
    - The kernel config page and shared page (0x1FF80000 / 0x1FF81000): firmware version, memory layout, and the time reference behind `osGetTime`. The clock starts at 2026-01-01, so runs stay deterministic.
    - QueryMemory over a region map, and CreateMemoryBlock.
    - FS: the SD card is a host folder (`--sdmc DIR`), and the app's RomFS is served from its `.3dsx` (`--romfs`). Covers files, directories and UTF-16 paths.
    - `cfg:u` (language, region, model), HID touch (`--touch`), and srv IsServiceRegistered.
    - The frame dump is both screens (top above bottom, 400x480).
    - GSP command 0x0C (TriggerCmdReqQueue) reports which GX commands an app wanted.
  - **Next:** the GPU. That's GX (MemoryFill, DisplayTransfer, TextureCopy, then command lists) and a PICA200. It's the decision point for reusing Azahar's video core, which would make the project GPLv2.
- **M8 done: our own GPU, part 1 (you chose to write it ourselves: no Azahar code, licence stays open).** `gpu/` emulates the PICA200 from the public docs (3dbrew; libctru/citro3d headers). Raw 3dbrew pages are fetched into `out/ref/3dbrew/` for reference.
  - **GX (`gpu/gx.c`, `pixel.c`):**
    - Commands: MemoryFill, DisplayTransfer (tiled <-> linear, formats, flip, 2x1/2x2 downscale), TextureCopy, RequestDma.
    - Pixel helpers: float24, 8x8 Morton tiling, and the five pixel formats.
    - GSP drains the GX queue on each trigger and raises PSC0/PSC1/PPF/P3D/DMA.
  - **Command lists (`gpu/regs.c`):**
    - Encoding: headers, byte masks, consecutive/FIFO writes, FINALIZE, and buffer jumps.
    - Data loads: shader code, operand descriptors, float uniforms (f24/f32), fixed attributes, immediate-mode vertices, and the lighting/fog/proctex/gas tables.
    - `GPU_TRACE=1` names every write (`gpu/regnames.c`, generated by `tools/gen_regnames.py`).
  - **Shader interpreter (`gpu/shader.c`):**
    - Coverage: the whole vertex ISA, including swizzles, masks, relative addressing, the call/if/loop stacks and bool/int uniforms.
    - Hardware quirks: inf*0 = 0, no -0, and NaN-aware min/max.
    - Arithmetic is float32: the hardware's float24 precision is not modelled yet.
  - **Pipeline (`gpu/draw.c`):**
    - Geometry: vertex arrays, index buffers, immediate mode, output semantics, triangles/strips/fans, and citro3d's mode-3 indexed triangles.
    - Clipping: x, y in [-w, w] and z in [-w, 0].
    - Rasteriser: viewport and depth map, a top-left-rule rasteriser, perspective-correct colour.
    - Fragments: the 6-stage colour combiner, alpha/stencil/depth tests, blending and logic ops, masks, scissor, culling.
    - Gotcha: the window's y = 0 is the last row of the tiled render target.
  - **Tests:** `make test-gpu` (469 checks). It covers float24, tiling, formats, the GX engines, command lists, a picasso-assembled shader exercising the ISA, a shared-edge square that must be drawn exactly once per pixel, and depth.
  - **Result:** simple_tri, both_screens, multiple_buf and immediate draw correctly, with frames recorded. The sweep is at 35 run / 19 stop / 8 build-failed. Every GPU stop is now named:
    - textures: 6 (including 2d_shapes: citro2d samples the procedural texture unit for every shape)
    - fragment lighting: 6
    - geometry shaders: 3
    - fog: 1
  - **Speed:** generated C is built unoptimised (`-O0`, sanitizers on), because clang's optimiser took ~144 s per example versus ~15 s. The sweep also builds runtime/hle/gpu once into `out/compat/liboslayer.a` and skips steps whose inputs are unchanged. A full sweep went from ~12 min to ~3.6 min, and to ~9 s when nothing changed.
  - **Next:**
    1. M9: textures (formats incl. ETC1, tiling, filtering, wrap).
    2. M10: fragment lighting, fog, procedural textures, geometry shaders.
    3. Later: float24-exact arithmetic; leaner generated C for optimised release builds (the giant jump-table switches slow clang down).
- **M9 done: textures, plus procedural textures (documented parts).** `gpu/texture.c`:
  - **Texture units 0-2:**
    - Formats: all 14 (incl. ETC1/ETC1A4), from 8x8 Morton tiles with t = 0 at the last memory row.
    - Sampling: nearest/linear, the four wrap modes and the border colour, mipmaps (LOD from screen-space derivatives + bias, clamped), and cube maps.
  - **Unit 3 (procedural):**
    - Implemented: the clamp modes, base shapes, mapping tables, colour table and separate alpha.
    - Noise and coordinate shifts stop by name: they aren't publicly documented, and we don't copy Azahar.
  - **citro2d:** uses unit 3 for anti-aliased circles ("circle" table) and for image tinting ("blend" ramp); its source is in `out/ref/citro2d` for reference.
  - **Bug found:** clamp-to-border returned texel 0 for *inside* coordinates, which made every citro2d sprite sample its transparent corner.
  - **Result:** textured_cube, cubemap, 2d_shapes, gpusprites and stereoscopic_2d render, so 9 GPU examples run. The remaining blockers are fragment lighting (6), geometry shaders (3), fog (1), procedural noise (1) and the system font (3).
- **M10 (in progress): fog and fragment lighting done.**
  - **Fog (`gpu/draw.c`):** a depth-indexed 128-entry table blends towards the fog colour.
  - **Lighting (`gpu/lighting.c`):**
    - Normals: per-pixel, from the interpolated quaternion or a bump map.
    - Lights: up to 8, positional or directional, with diffuse/ambient, specular0 * D0 + specular1 * reflection * D1, spotlight and distance-attenuation tables, and Fresnel into alpha.
    - Lookup tables: the environment configurations, input selectors, abs flags and scales.
    - Not done (named stops): shadows, geometric factors, tangent maps and cos(phi).
  - **Result:** fragment_light, lenny, toon_shading, normal_mapping, wide_mode_3d and qtm/lenny_qtm_movement_naive render lit 3D. 16 GPU-using examples run.
  - **Geometry shaders, point mode:**
    - The interpreter does SETEMIT/EMIT. The vertex shader's outputs fill the geometry shader's input buffer, and every full buffer runs it once.
    - Emitted triangles are drawn, with the winding flag honoured. The mode is GSH_MISC0's low byte.
    - Result: geoshader renders.
  - **Left:**
    - Geometry-shader variable-size (loop_subdivision) and fixed-size/particle (particles) modes stop by name.
    - Procedural noise is undocumented and stays a named stop. The system font is firmware.
- Run everything with `make test`, inside WSL (`wsl -d Ubuntu-24.04 -u root --cd /mnt/c/any3ds -- bash -lc 'make test'`).

## Milestone 0: recompile one function

Input `tests/m0/test.c`:

```c
int test(int a, int b) { return (a + b) * 3; }
```

Build it as a bare ARM object, without libctru:

```
arm-none-eabi-gcc -marm -mcpu=mpcore -O1 -c tests/m0/test.c -o tests/m0/test.o
```

Then:

1. `python -m recomp tests/m0/test.o -o out/gen.c` finds `test` in the symbol table, disassembles it, and emits C.
2. Write a host harness `tests/m0/harness.c` that allocates a context, sets `r0 = 2` and `r1 = 5`, calls the generated function, and asserts `r0 == 21`. Also test a few edge cases (negatives, overflow).
3. Add a `make test` (or a script) that runs the whole chain.

Only implement the instructions this function actually compiles to (check with `arm-none-eabi-objdump -d`). It's probably just `add`, an `add` with a shifted operand, and `bx lr`. Barrel-shifter operands (`lsl/lsr/asr/ror #imm`) are worth doing properly now.

**Done when:** `test(2, 5)` returns 21 from recompiled code running on x86-64.

## Roadmap after M0

1. **M1:** branches, loops, compares, condition flags and conditional execution. Multiple functions, plus `bl` calls between them.
2. **M2:** loads/stores (`ldr/str/ldrb/ldrh`, `ldm/stm`, `push/pop`) and a stack in guest memory.
3. **M3:** differential testing. Run each test both through recompiled code and through a reference (Unicorn engine, or Azahar's interpreter), then compare registers and memory after every block. This is the main bug-catching tool, so build it early.
4. **M4:** Thumb mode and ARM/Thumb interworking (`bx` with bit 0 set).
5. **M5:** VFP floating point.
6. **M6:** a full libctru homebrew app, with `SVC` routed to a stub OS layer, then to Azahar's HLE.
7. **Later:** cycle counting per block (Azahar's timers and vblank depend on it), CRO relocation, an indirect-jump table + interpreter fallback, a Metal renderer via MoltenVK, and the iOS shell app.

## How to work on this

- Small steps. Get each milestone passing before adding instructions it doesn't need.
- Write the test first, then the translation.
- Keep the generated C readable. Optimizing is clang's job, not ours.
- Ask before adding new dependencies or restructuring the layout.
- Explain ARM-specific gotchas when you hit them (flags, PC-relative reads, shifts by register, etc.). The owner is learning this as we go.
