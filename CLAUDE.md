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
- **M6b started: the recompiled hello world runs.** The ELF entry point (libctru crt0's `_start`, a plain label) now seeds function discovery and is exported as `recomp_entry`. `make run-m6` (`tests/m6/run_hello.c`) starts it with a stack and TLS block, answers the first kernel calls, and logs every svc:
  - **Calls answered so far:** CreateAddressArbiter, GetResourceLimit and its limit/current values, CloseHandle, ControlMemory (a 30 MiB heap at 0x08000000 and a 32 MiB linear heap at 0x14000000), and handles for mutexes and events.
  - **First cut of IPC:** ConnectToPort and SendSyncRequest work through the command buffer at TLS + 0x80. `srv:` answers RegisterClient, EnableNotification and GetServiceHandle; other services get a bare success reply, and every request is logged.
  - **Where it stops today:** with threads recorded but not run and waits returning at once, the sequence is:
    1. srv:RegisterClient, then srv:GetServiceHandle("APT:S") before every APT call.
    2. APT GetLockHandle (0x01) → Initialize (0x02) → CreateThread (the APT event thread at 0x001070a4) → Enable (0x03) → NotifyToWait (0x43).
    3. GlanceParameter (0x0D) **in a loop**, waiting for the wakeup parameter that a real APT sends.

    **Next:**
    1. Threads (host threads or a cooperative scheduler).
    2. Real APT replies: hand out the wakeup parameter.
    3. Then GSP (shared memory, framebuffers), HID and FS. Azahar's HLE services are the likely source.
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
