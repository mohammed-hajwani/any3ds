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
tests/
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
