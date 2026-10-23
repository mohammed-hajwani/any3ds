# any3ds

A **static recompiler** for Nintendo 3DS code. It translates 32-bit ARM (ARMv6K) machine code ahead of time into C, which is then compiled into native code. The end goal is running 3DS software at native speed on iOS.

Why ahead of time? iOS doesn't allow JIT compilation, so 3DS emulators on iPhone fall back to slow interpreters. If the whole program is translated before it runs, nothing has to be generated at runtime.

> **Status:** a personal learning project, now running a commercial game. Milestones M0-M10 built the translator (ARM, Thumb, VFP, real-world control flow, checked instruction by instruction against Unicorn), our own minimal OS layer and our own PICA200 GPU (no third-party emulator code): 49 of devkitPro's 62 examples run ([docs/compat.md](docs/compat.md)). Since then it recompiles retail games from the user's own dump (`code.bin` + CRO modules, no symbols) and **Pokémon X boots into its game world**: language select, title screen, Professor Sycamore's intro, name entry, and the bedroom in Vaniville Town, all in 3D, with its AAC music streams handled by our DSP. It renders in software on all CPU cores (about 15 fps in heavy 3D scenes, near 60 without drawing); a hardware renderer is planned. Development targets x86-64 Linux (WSL2); iOS comes much later.

## Progress

![What we cover: OS service commands, CPU instructions and GPU shader instructions; green = implemented](docs/progress.svg)

One cell per item, green when implemented: every command 3dbrew documents for each OS service (including services we don't have at all yet, so the grey is what's left), every ARMv6K + VFPv2 instruction the CPU can execute, and every PICA200 shader instruction. Generated from 3dbrew's command lists and our own source by `make progress` (`tools/progress.py`); hover a cell for its name.

## How it works

```
ARM ELF ──► load symbols ──► disassemble (Capstone) ──► translate each instruction to C ──► clang ──► native code
```

Each guest function becomes a C function that operates on a CPU context:

```c
/* gcd */
void f_00100044(Ctx *ctx)
{
    const uint32_t ret = ctx->r[14];  /* return address we were called with */
    /* 00100044: cmp r0, #0 */
    sub_flags(ctx, ctx->r[0], 0u);
    /* 00100048: beq #0x10006c */
    if (cond_eq(ctx)) { goto L_0010006c; }
    ...
L_00100054:
    /* 00100054: cmp r0, r1 */
    sub_flags(ctx, ctx->r[0], ctx->r[1]);
    /* 00100058: subhi r0, r0, r1 */
    if (cond_hi(ctx)) { ctx->r[0] = ctx->r[0] - ctx->r[1]; }
    ...
```

- Each instruction keeps its address and original disassembly as a comment, so the generated code can be debugged.
- **An unsupported instruction is a hard error** naming its address and mnemonic. Nothing is ever skipped silently.
- Optimisation is clang's job. Generated code should stay readable.

OS calls (`SVC`) go to `hle/`, our own high-level emulation of the 3DS kernel and services written from the public [3dbrew](https://www.3dbrew.org) docs: threads and synchronisation, IPC, APT, GSP, HID, FS (RomFS, saves, extdata), CFG, RO (CRO modules), Y2R and the DSP (audio inputs and an AAC decoder). The GPU in `gpu/` is our own too. No emulator code is reused, so the licence stays open. Where 3dbrew is silent, behaviour comes from what the game's own code expects, and each such guess is logged when it is used. See [CLAUDE.md](CLAUDE.md) for the full design and roadmap.

## Legal rules (read before contributing)

- **Never commit** game code, ROMs, dumps, decryption keys, firmware, or recompiled output of commercial games. Keep all of it in `dumps/` and `out/`, which are gitignored.
- **Never download, search for, or link to** ROMs or keys, including in issues and PRs.
- Metadata is fine: compatibility lists, per-game configs (addresses, patch points) and dump hashes.
- If any Azahar code is reused, the project becomes GPLv2.

## Setup

Everything builds inside **WSL2 Ubuntu 24.04** on Windows, or on native Linux.

1. **WSL distro**, from PowerShell:
   ```bash
   wsl --install -d Ubuntu-24.04
   ```
2. **Host tools**, inside Ubuntu:
   ```bash
   sudo apt update && sudo apt install -y clang make python3-venv git libunicorn-dev
   ```
3. **devkitPro** (devkitARM and libctru): follow the Linux instructions at <https://devkitpro.org/wiki/Getting_Started>, then run:
   ```bash
   sudo dkp-pacman -S 3ds-dev
   ```
   Open a new shell afterwards so `$DEVKITARM` is set.
   *Gotcha:* `apt.devkitpro.org` sits behind Cloudflare, so a plain `wget` of the installer script returns 403. Download the script in a browser instead.
4. **Clone and run the tests:**
   ```bash
   git clone https://github.com/mohammed-hajwani/any3ds.git
   cd any3ds
   make venv    # Python venv with capstone + pyelftools
   make test
   ```

When the WSL distro runs as root, you can run the tests straight from Windows:
```bash
wsl -d Ubuntu-24.04 -u root --cd /mnt/c/any3ds -- bash -lc "make test"
```

`make run-m6` runs the recompiled hello world, presses START at frame 10 and saves both screens to `out/m6/frame.bmp`.

### Running your own game dump

Only ever your own, decrypted dump, kept in `dumps/` or outside the repo (never committed):
```bash
tools/build_dump.sh /path/to/your/game.3ds out/game     # recompile everything (~8 min); OS_ONLY=1 rebuilds just the OS layer
out/game/run --romfs /path/to/your/game.3ds --save out/game/save --live out/game/live.bmp --input out/game/keys.txt --speed 1
```
`--live` saves both screens every 30 frames and `tools/live.html` (served by `tools/live_server.py`) shows them with buttons that write `--input`. `--speed 1` plays at the 3DS's own speed (0 = as fast as possible). Runs are deterministic: `--hold FRAME:KEYS` and `--touch FRAME:X,Y` replay a session exactly. `GPU_SKIP_UNTIL=N` fast-forwards to frame N without drawing; `GPU_TRACE=N` and `HLE_TRACE=N` trace the GPU or the OS from frame N; `GPU_THREADS=N` sets the drawing threads.

`make compat` builds, recompiles and runs every example in devkitPro's `3ds-examples` (installed with `3ds-dev`) and writes the table in [docs/compat.md](docs/compat.md). It takes about 4 minutes, and seconds when nothing changed.

## Repo layout

```
recomp/            Python recompiler
  elf_loader.py    read ELF function symbols and their bytes
  disasm.py        Capstone wrapper (ARM mode; Thumb later)
  translate.py     instruction -> C statement(s)
  emit_c.py        write functions + symbol table to a .c/.h pair
  __main__.py      CLI: python -m recomp <input.elf> -o out/gen.c
runtime/
  context.h        CPU context, flag/condition helpers, memory access
  runtime.c        guest memory (4 GiB mmap), error handlers
hle/               the OS layer: high-level emulation of the 3DS kernel and services
  kernel.c         kernel objects, handles, threads + scheduler, virtual clock
  svc.c            svc number -> kernel call; memory map
  ipc.c            IPC: ports, sessions, request dispatch
  srv.c apt.c gsp.c hid.c fs.c cfg.c ro.c y2r.c dsp.c   one file per service
  config_mem.c     the kernel config page and shared page (versions, clock)
  framebuffer.c    3DS framebuffer -> upright .bmp
gpu/               our own emulation of the 3DS GPU (PICA200), from the 3dbrew docs
  gx.c pixel.c     2D engines (fill, display transfer, texture copy), formats, tiling, float24
  regs.c           command lists: register writes and the data FIFOs
  shader.c         vertex shader interpreter
  draw.c           vertex fetch, clipping, rasteriser, colour combiners, tests, blending
tests/
  m0/ ... m5/      one directory per milestone
  real/            -O2 code + libgcc; extends, DSP multiplies, saturation, unusual control flow
  m6/              libctru programs run on the HLE: hello world, threads, deadlock
  compat/          the compatibility sweep: example list, expected outcomes, frame hashes
  expect_reject.sh asserts the recompiler refuses bad input
dumps/  out/       gitignored: your own dumps and all generated code
```

## Testing approach

- **Tests come first.** Each milestone adds test programs under `tests/mN/` and only the instructions those programs actually compile to. Check with `out/mN/*.dis`.
- **Differential tests:** the same C source is compiled for ARM and recompiled, and also compiled natively for the host. The harness compares results over edge-case and random inputs.
- **Hand-written assembly** covers corners C rarely reaches, such as flag semantics, `lsr #32` and `rrx`, checked against an independent reference model.
- **Negative tests** make sure unsupported code is rejected rather than miscompiled.
- **Whole programs:** real libctru programs run on the HLE and are checked by their output (a golden debug log, or a hash of the frame on screen).
- Harnesses build with `-Wall -Wextra -Werror` and ASan/UBSan.

## Roadmap

| | Milestone | |
|---|---|---|
| M0 | Recompile one function | ✅ |
| M1 | Branches, loops, flags, conditional execution, `bl`, push/pop | ✅ |
| M2 | Loads/stores (`ldr/str/ldrb/ldrh`, `ldm/stm`), literal pools, switch tables | ✅ |
| M3 | Differential testing against a reference CPU (Unicorn), instruction by instruction | ✅ |
| M4 | Thumb mode and ARM/Thumb interworking, register shifts, mul/mla | ✅ |
| M5 | VFP floating point (VFPv2, hard-float ABI, exact ARM NaN rules) | ✅ |
| — | Real-code readiness: -O2 + libgcc, indirect calls, tail calls, function discovery | ✅ |
| M6a | Recompile a real libctru homebrew app in full (303 functions, RunFast VFP, atomics, svc hook) | ✅ |
| M6b | OS layer: kernel (threads, sync, scheduler), IPC, APT/GSP/HID/FS; the hello world runs and draws its text | ✅ |
| M7 | Compatibility sweep over devkitPro's 62 examples: 31 run, the rest need the GPU, firmware or extra libraries | ✅ |
| M8 | Our own GPU, part 1: GX engines, command lists, vertex shaders, rasteriser, colour combiners, blending, depth | ✅ |
| M9 | GPU textures (14 formats incl. ETC1, filtering, mipmaps, cube maps) and procedural textures; citro2d works | ✅ |
| M10 | Fog, fragment lighting and geometry shaders (point, fixed- and variable-size modes) | ✅ |
| M11 | Retail code: `code.bin` + CROs from your own dump, function discovery without symbols | ✅ |
| M12 | Audio DSP: voices consumed at the right rate (silent), an AAC decoder (decodes to silence) | ✅ |
| — | Pokémon X boots into its game world; 7x faster (`-O1` generated code), drawing on all cores | ✅ |
| next | The game's script engine, sound output, a hardware renderer (Vulkan, later Metal), savestates | |
| later | Cycle counting, interpreter fallback for unresolved jumps, iOS shell app | |

## Contributing

1. Branch off `main`, keep changes small, and make sure `make test` passes.
2. A new instruction needs a test that exercises it, including its flag behaviour if it sets flags.
3. Ask before adding dependencies or restructuring the layout.
4. Explain ARM-specific gotchas in comments where you handle them (flags, PC-relative reads, shift edge cases, and so on).

## License

Not chosen yet. If Azahar code ends up being reused, it will be GPLv2.
