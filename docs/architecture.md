# Architecture and design decisions

Every structural choice in this project, why it was made, what the alternatives
were, what each one costs, and where the lever is if you want to change it.

Read it as a set of decision records. Each one is:

- **What we do** — the choice as the code actually implements it.
- **Why** — the reasoning that picked it.
- **Instead we could have** — the real alternatives, including the ones most
  other projects pick.
- **What it costs** — the bill that comes with the choice. Every choice has one.
- **Lever** — what you would change, and what you'd get.

Nothing here is sacred. The point of writing it down is that you can see which
beam is load-bearing before you pull on it.

---

## 0. The one big bet

**What we do.** Translate the game's ARM code into C *before* it ever runs, one
C function per guest function, and compile that C into the app. At run time
nothing is generated: the app is the game.

**Why.** iOS forbids JIT compilation (no `mmap` with `PROT_EXEC` on writable
pages, no `mprotect` to executable without the right entitlement). Every 3DS
emulator on iPhone therefore runs an interpreter, which costs 20-100 host
instructions per guest instruction. If the translation happens ahead of time,
the cost is ~1-3 host instructions per guest instruction and clang's optimiser
gets to see the whole program.

**Instead we could have:**

| Approach | Why not |
|---|---|
| **Interpreter** (what iOS emulators do today) | 20-100x slowdown; the thing we are trying to beat. |
| **JIT / dynarec** (Azahar, Citra, every desktop emulator) | Forbidden on iOS. Also needs a code cache, invalidation, and a compiler at run time. |
| **LLE: run the console's own firmware** on an emulated ARM11 | Needs the user's firmware and keys; a legal and practical non-starter, and it does not remove the need for a CPU core. |
| **Decompile to source-level C** (a human-readable port) | Years per game, per game. Static recompilation is per *platform*. |
| **AOT to LLVM IR instead of C** | A real option, see §3.3. We chose C for the free optimiser and debuggability. |

**What it costs.** Three structural prices, and all of this project's hard
problems are downstream of them:

1. **You must find all the code before you run it.** A JIT discovers code by
   executing it; we have to discover it by reading it (§3.5). Anything we
   cannot find statically simply is not there at run time.
2. **No self-modifying or dynamically generated guest code.** Code patched at
   run time, decompressed at run time, or generated at run time has no
   translation. (3DS CRO modules are handled by recompiling them ahead of time
   too, §3.8 — but only the modules that exist in the dump.)
3. **Per-game builds.** The output is C that must be compiled and linked. On
   iOS that means: the user recompiles their own dump on a computer, builds, and
   sideloads one app per game. That is the shape of the end product; it is worth
   being explicit about it now rather than discovering it at the end (§8, §9.1).

**Lever.** The escape hatch for all three is an **interpreter fallback**: when
an indirect jump lands somewhere we have no function for, interpret from there
until control reaches a known entry point. An interpreter is not JIT, so it is
perfectly legal on iOS. We don't have one (today `recomp_bad_call` aborts). It
is the single highest-value missing piece — see §10.4.

---

## 1. The pipeline

```
   dump (.3ds/.cxi/code.bin)          homebrew ELF
            |                              |
       ctr_loader.py                   elf_loader.py        <- segments, (maybe) symbols
            |                              |
        discover.py  <--- no symbols ---  (symbols: direct)  <- what is a function, what is code
            |                              |
            +------------ disasm.py (Capstone) --------+     <- ARM / Thumb instructions
                                 |
                           translate.py                      <- one instruction -> C statements
                                 |
                            emit_c.py                         <- functions + tables + program image
                                 |
                       out/gen.c  +  gen.h
                                 |
      clang  +  runtime/ (CPU support)  +  hle/ (the OS)  +  gpu/ (the PICA200)
                                 |
                          a native binary
```

Four layers, each with its own licence risk, its own test strategy, and its own
performance profile:

| Layer | Language | Runs | Tested by |
|---|---|---|---|
| `recomp/` | Python 3 | ahead of time, on a PC | its own output being correct |
| generated C + `runtime/` | C | per guest instruction | Unicorn lockstep diff (M3) |
| `hle/` | C | per OS call | golden logs, frame hashes |
| `gpu/` | C | per draw | 469 unit checks + frame hashes |

The split is deliberate: the slow, clever, rewrite-friendly part is Python and
runs offline; everything on the hot path is plain C.

---

## 2. Layer 1: the recompiler

### 2.1 Python for the translator

**What we do.** `recomp/` is Python 3 with `capstone` and `pyelftools`.

**Why.** It is offline, so its speed does not matter; and it is the part of the
codebase that gets rewritten most often as new instructions and new code shapes
turn up. Python's dicts-and-dataclasses style makes "add an instruction" a
five-line change.

**Instead we could have** written it in C++ or Rust (and eventually have the
recompiler run *on the device*, which Python makes awkward), or written it as an
LLVM pass.

**What it costs.** Recompiling a retail game is minutes, not seconds; and
`tools/build_dump.sh` has to shard the work with `xargs -P`. Also, no type
checking on a 1700-line translator.

**Lever.** If you ever want "user drops a dump into the app and it builds
itself", the translator has to move out of Python. Until then this is a
non-issue. Worth doing sooner: `mypy`/`ruff` in CI, and splitting
`translate.py` (§10.3).

### 2.2 Capstone as the decoder

**What we do.** `disasm.py` wraps Capstone, with `detail = True` for operands,
shifts and condition codes, and rejects anything Capstone decodes that ARMv6K
does not have (32-bit Thumb that isn't `bl`/`blx`).

**Why.** A complete, battle-tested ARM decoder for free, plus disassembly text
that goes straight into the generated code as comments — which is what makes the
output debuggable.

**Instead we could have** written our own table-driven decoder from the ARM ARM.

**What it costs.** Capstone is a *disassembler*, not a semantic model, and it is
wrong in ways that matter. The code already works around:

- `update_flags` is true for `adc`/`sbc`/`rsc` without `S` (they read C, they
  don't write flags) → we read the S bit (bit 20) out of the encoding instead.
- `post_index` is always false for `ldrh`/`strh`/`ldrsb`/`ldrsh` → we read the
  P bit (bit 24) ourselves.
- It happily decodes Thumb-2 and iWMMXt, neither of which exists on this CPU.
- Capstone 6 renamed the constants we use, hence the `capstone>=5,<6` pin.

So `translate.py` already reaches into `insn.bytes` for ground truth in several
places. That is the smell: we pay for a decoder and then double-check it.

**Lever.** A ~600-line ARMv6K decoder producing an explicit record (operands,
S bit, P/U/W bits, cond) would delete every workaround, remove the version pin,
and make the translator's input trustworthy. Medium effort, and the Unicorn
lockstep harness makes it safe to attempt. Keep Capstone purely for the comment
text.

### 2.3 No IR: decode straight to C text

**What we do.** `translate.py` maps one instruction to a list of C source lines,
immediately. `_HANDLERS[insn.id]` → a function returning `['ctx->r[0] = ...;']`.
Conditional execution wraps those lines in `if (cond_eq(ctx)) { ... }`.

**Why.** It is the shortest path from "I read the ARM ARM" to "it works", and the
output is readable, which was a stated goal. Optimisation is clang's job.

**Instead we could have** built an IR (a list of typed operations with explicit
register reads/writes) and emitted C from that, or emitted LLVM IR directly.

**What it costs.** This is the most consequential "cheap now, expensive later"
decision in the project, for three reasons:

1. **No analysis is possible.** We cannot ask "is the carry flag this `cmp`
   computes ever read?" because by the time we would ask, the answer is a
   string. So every flag-setting instruction writes all four flags to memory,
   always. ARM code sets flags constantly and reads them rarely.
2. **No register allocation.** Every instruction reads and writes `ctx->r[n]`
   in memory (§4.1). A per-function pass over an IR could keep them in C locals.
3. **One backend only.** C. An IR would let you add an LLVM-IR backend later
   (faster to compile than giant C functions, and far better control over flags).

**Lever.** Introducing an IR is the enabling refactor for nearly every
performance idea in §10. It does not have to be fancy: a dataclass per operation
with `reads`, `writes`, and a `to_c()` method would already allow NZCV liveness
and register localisation. Do this before you do any other performance work.

### 2.4 The unit of translation is the guest function

**What we do.** Each guest function becomes one `void f_XXXXXXXX(Ctx *ctx)`, named
by its guest address. Branches inside it become `goto L_XXXXXXXX;`.

**Why.** It lines up the host call stack with the guest call stack (§4.4), it
gives clang a function-sized region to optimise, and it keeps the output
navigable: a crash in `f_00123456` tells you exactly where you are in the game.

**Instead we could have:**

- **One C function per basic block**, with a central dispatch loop. This is what
  dynarecs do, and it handles *any* control flow — computed jumps, longjmp,
  exceptions, thread switches — because the "program counter" is a real variable.
  It costs an indirect call per block (so maybe 2-5x slower) and loses most
  cross-block optimisation.
- **One giant function for the whole program** with a switch over the PC. clang
  would die; and it was already measured that big switches are what make clang
  slow (§7.7).
- **Tail-call chaining** (`[[clang::musttail]]` from block to block). Elegant,
  no dispatch loop, but it needs guaranteed tail calls on every target and it
  gives up the mirrored call stack.

**What it costs.** Everything that does not fit "functions call functions and
return to their caller" has to be special-cased, and §4.4 is the list of those
special cases. It is a real list and it grew with every new code generator we
met (gcc, then libgcc hand-written asm, then newlib, then ARMCC in a retail
game).

**Lever.** Keep it. It is the right default and the reason the generated code is
fast and debuggable. But pair it with the interpreter fallback (§10.4) so the
cases that do not fit degrade instead of aborting.

### 2.5 Function discovery

**What we do.** Two modes.

*With symbols* (homebrew ELF): the symbol table gives starts, sizes, and the
Thumb bit; `$a`/`$t`/`$d` mapping symbols give code-vs-data. On top of that,
`discover_functions` adds every call/branch target that is not already a known
function start, because hand-written asm jumps into the *middle* of other
functions (libgcc's alternate entry points). Such a target becomes its own
function from there to the end — the code is simply **recompiled twice**.

*Without symbols* (retail `code.bin`, 990 lines of `discover.py`): recursive
descent from the entry point, plus:

- `bl`/`blx #imm` targets are new functions; `blx` flips ARM/Thumb.
- a `b` to a known start is a tail call; a forward `b` stays inside the function
  however far it goes (compilers move cold blocks past other functions); a `b`
  backwards below our own start is a tail call into a new function.
- pc-relative loads mark their target words as literal data, so pools are never
  decoded as instructions.
- **function pointers**: every word in data or a literal pool that points into
  the code is a *candidate*; a candidate is accepted only if everything
  reachable from it decodes cleanly and the translator accepts it. Rejects are
  listed in `OUTPUT.discovery.txt`, never compiled.
- switch-table shapes are recognised per compiler: gcc's `add pc, pc, rX, lsl #2`,
  ARMCC's `cmp rX, #N; ldrlo pc, [pc, rX, lsl #2]`, `ldr pc, [pc, #-4]` veneers,
  table-plus-offset in hand-written asm, and `mov pc, rX`.
- it all runs **to a fixpoint**, because a newly found start can shorten another
  function and newly found data can stop a fall-through.

**Why.** Retail games have no symbols at all. Without this there is no path to
commercial software, and 3dsx homebrew only gets you so far.

**Instead we could have:**

- **Linear sweep disassembly** (decode everything, hope for the best): it
  decodes literal pools as instructions and produces garbage.
- **Trace-driven discovery**: run the game in Azahar, log every executed
  address, recompile those. Much higher confidence per function, and it finds
  code that static analysis cannot (computed jumps with runtime-dependent
  targets). But it only finds what that playthrough touched, so coverage is
  bounded by testing, and it makes the toolchain depend on another emulator.
- **Accept only what we are sure about and interpret the rest** — the hybrid.

**What it costs.** Discovery is heuristic, and heuristics are where silent
wrongness lives. The mitigations in place are good ones: every guess must
*verify* (decode + translate cleanly), rejects are reported, and the same code
being recompiled twice is tolerated rather than fought. But a function nobody
points at from data or code is invisible, and a pointer built arithmetically at
run time cannot be followed.

**Lever.** Two things make this much stronger. (1) The interpreter fallback
turns "discovery missed it" from a crash into a slow path. (2) A trace from
Azahar used as a *cross-check* rather than a source: run the sweep, compare the
set of executed addresses with the set we discovered, and report the difference.
That gives you a coverage number instead of a feeling.

### 2.6 Code versus data

**What we do.** Only bytes we have a positive reason to believe are code get
decoded: mapping symbols with symbols, reached-by-descent without. Literal pools
inside functions are emitted as comments (`literal pool .word 0x...`), and a
pc-relative load of a word in a **non-writable** segment is folded into a
constant in the generated C, with the value shown.

**Why.** Folding literals is free performance and makes the output readable
(`ctx->r[0] = 0x1000u; /* ldr r0, [pc, #12] */`). Restricting it to read-only
segments is the correctness condition: nothing can change it at run time.

**Instead we could have** read every literal from guest memory at run time
(always correct, one load each), or folded everything regardless of segment
(wrong if the data is writable).

**What it costs.** A game that patches its own read-only data before use (or a
loader that relocates text) would see stale constants. For CRO modules this is
handled explicitly: `cro.without_patched()` removes the words the loader will
patch from the foldable set.

**Lever.** Fine as is. If you ever meet a game that rewrites its own rodata,
make folding opt-out per range rather than abandoning it.

### 2.7 An unsupported instruction is a hard error

**What we do.** No silent skipping, anywhere, ever. Unknown instruction →
`UnsupportedInstruction` naming address and mnemonic, and the run writes no
output at all. `--keep-going` exists for exploring a big dump: the function
becomes a stub that aborts loudly *if called*, and everything is listed in
`OUTPUT.failures.txt`.

**Why.** A skipped instruction is a bug that appears thousands of
instructions later as corrupted state. This single rule is why the project's
bugs are findable.

**Instead we could have** emitted a no-op (fast progress, undebuggable), or
dropped to an interpreter per instruction (the hybrid again — correct *and*
progress, which is why §10.4 matters).

**What it costs.** Coverage comes in at the pace of the instruction set. The
progress map (`make progress`) exists because of this choice.

**Lever.** Keep the rule absolutely. Add the interpreter *underneath* it, so
"unsupported" means "slow" rather than "stop", while still being reported.

### 2.8 Relocatable output for CRO modules

**What we do.** 3DS games load "DLLs" (CRO modules) at run time at an address
they choose. We recompile each module ahead of time into code that is
*position-independent at the C level*: every address derived from where the code
sits (pc reads, `adr`, return addresses, switch-case labels) is emitted as
`(RB + 0x...)`, where `RB` is a global the OS layer sets when the game loads the
module (`hle/ro.c` → `recomp_module_attach`). `recomp_find` then searches the
attached modules' offset tables as well as the main program's.

**Why.** It is the only way to keep ahead-of-time translation for code whose
address is not known until run time.

**Instead we could have** forced a fixed load address (games choose it, so no),
interpreted module code (needs an interpreter), or re-recompiled at load time
(JIT; forbidden).

**What it costs.** A runtime add on every code-derived address; `switch` labels
have to subtract `RB` to stay compile-time constants; and the set of modules
must be known when you build. A patched or DLC module that is not in the dump
you built from has no code.

**Lever.** Fine. Note that `MAX_ATTACHED` is 64 and lookup walks the attached
list linearly before binary-searching inside one — if a game loads many modules
and calls across them constantly, that is a measurable cost (§10.5).

---

## 3. Layer 2: the execution model

This is the heart of the project: what a guest instruction *becomes*.

### 3.1 All CPU state in one struct, passed as a pointer

**What we do.** `Ctx` in `runtime/context.h`: `r[16]`, the four flags as
separate `uint32_t`s, the VFP file as a union of views, `fpscr`, `ge`, `tls`,
the exclusive monitor, the tick budget, the tail-return override, and `mem`.
Every generated function is `void f_XXXXXXXX(Ctx *ctx)`.

**Why.** One uniform signature makes indirect calls, thread creation, the
interpreter boundary and the test harnesses all trivial: a guest function
pointer is just a `GuestFn`. And a struct is honest: on the CPU those registers
*are* memory-like state shared by everything.

**Instead we could have:**

- **Recovered the ABI**: emit `uint32_t f(uint32_t r0, uint32_t r1, ...)` and
  return in r0, i.e. make guest functions look like C functions. Much faster
  (arguments stay in host registers, no context at all for leaf functions), and
  it is what a decompiler does. But it requires knowing each function's real
  signature, and hand-written asm, varargs, and functions that return through
  registers other than r0 break it. One wrong guess is silent corruption.
- **Globals instead of a pointer**: a single static `Ctx`. Slightly cheaper
  addressing, but then you cannot have two contexts, which kills both
  multithreading (§5.2) and the differential harness.
- **Hot registers as parameters, the rest in the struct**: a middle ground
  (pass r0-r3 and sp as values, spill into the struct on calls).

**What it costs.** Every single guest instruction is at least one load and one
store through a pointer, unless clang can prove it may keep the value in a host
register. And **clang usually cannot prove it across a guest memory store**,
because a write through `ctx->mem` could, as far as the compiler knows, alias
`ctx->r[]` itself. So the pattern

```c
ctx->r[0] = ctx->r[0] + ctx->r[1];   /* add r0, r0, r1 */
write32(ctx, ctx->r[3], ctx->r[0]);  /* str r0, [r3]   */
ctx->r[2] = ctx->r[0] + 1;           /* add r2, r0, #1 */
```

very likely reloads `r0` after the store. On a load/store-heavy game that is a
large constant factor. **This is a hypothesis, not a measurement** — there is no
benchmark in the repo yet (§10.1), so measure before you optimise.

**Lever (the big one).** *Register localisation*: at function entry, copy the
registers this function uses into C locals; operate on locals; write them back
before anything that can observe the context (a call, an svc, a trap, a return,
an indirect jump). Locals whose address is never taken cannot alias anything, so
clang keeps them in host registers with no aliasing promise needed. This is
standard practice in static recompilers and is usually worth several times more
than any other single change. It needs the IR from §2.3 to know which registers
a function touches and where the flush points are.

### 3.2 Guest memory is a flat 4 GiB mapping

**What we do.** `guest_mem_alloc()` mmaps the whole 32-bit address space
(`MAP_NORESERVE`, so pages materialise on touch). A guest access is
`ctx->mem + (uint32_t)addr`, via `memcpy` so unaligned access works as it does
on the 3DS. Accesses in the last few bytes of the space take a slow path that
wraps to address 0 — a bug the M3 differential test found.

**Why.** It makes a guest load one host load. No page tables, no bounds check,
no TLB, nothing. The 3DS has 128 MB of memory; mapping 4 GB of mostly untouched
address space costs nothing but VA.

**Instead we could have:**

- **A software MMU** (page-table lookup per access, as most emulators do):
  correct faults, memory protection, and MMIO hooks — at 5-10x the cost per
  access.
- **Base-register + 32-bit offset with a fixed base address**: map at a constant
  host address so the base is an immediate rather than a load from `ctx->mem`.
  On arm64 you would pin the base in a reserved register.
- **Host-page-protection tricks**: `mprotect` guest regions to catch faults in a
  SIGSEGV handler. Fast and precise, but signal handlers and iOS do not mix
  well.

**What it costs.** No memory protection at all: a guest null-pointer write
silently scribbles on guest address 0 instead of faulting, and guest bugs become
mysterious rather than immediate. No MMIO: anything that must be *observed*
(GPU registers, shared memory with a service) has to be polled or driven from
the OS layer rather than trapped. And 4 GB of VA per context, which matters if
you ever want many contexts.

**Lever.** Keep it. Two cheap improvements: leave guest page 0 unmapped
(`PROT_NONE`) so null writes fault immediately with a useful backtrace, and
consider a fixed map address so the base is a constant.

### 3.3 Flags are four words, computed eagerly

**What we do.** `n`, `z`, `c`, `v` as separate `uint32_t`s holding 0 or 1.
Every flag-setting instruction calls a helper (`addc_flags`, `sub_flags`,
`set_nz`); conditions are `cond_eq(ctx)` etc. All arithmetic reduces to
`a + b + carry`, which is how ARM actually computes subtraction (`a - b` is
`a + ~b + 1`), so C after a subtract means "no borrow" — the opposite of x86.

**Why.** Separate words are branch-free to set and trivially readable; one
`addc_flags` makes every add/sub/adc/sbc/rsb/rsc/cmp/cmn correct by
construction, including the overflow rule. The alternative encodings (a packed
CPSR, or host flags) are faster but much harder to get exactly right, and this
is the part of an ARM translator where "nearly right" means weeks of debugging.

**Instead we could have:**

- **Lazy flags** (store the operands and the operation; compute N/Z/C/V only
  when a condition is actually read). Every dynarec does this; it removes most
  flag work entirely. Needs an IR and liveness.
- **A packed CPSR word** with bit twiddling: fewer stores, worse readability.
- **Host flags** (compile the guest's flags onto the host's, as a dynarec can):
  impossible from C.

**What it costs.** Four stores per flag-setting instruction, and ARM code sets
flags on most instructions while reading them on few. With state in memory
(§3.1) clang cannot always eliminate the dead ones.

**Lever.** Dead-flag elimination: a backwards pass per function marking, for
each flag-setting instruction, whether any reachable instruction reads that flag
before overwriting it; skip the ones nobody reads. Combined with localisation
(§3.1) this is probably the second-biggest win available. Both need §2.3.

### 3.4 The host call stack mirrors the guest call stack

**What we do.** `bl` → a C call. `bx lr` → `RETURN_TO(ctx->r[14])`, which checks
you are returning to the address you were called with and then `return`s. The
guest's own stack in guest memory still holds everything the guest pushed
(including `lr`), so both stacks exist and agree.

**Why.** It is what makes the generated code *native*: the host's `bl`/`ret`
(with its return-address prediction) does the work, clang can inline across it,
and a host backtrace is a guest backtrace.

**Instead we could have** kept the PC in a variable and run a dispatch loop
(§2.4) — which handles everything but costs an indirect branch per block.

**What it costs.** Real guest code does not respect the rule, and each violation
needed its own mechanism. This list *is* the design:

| Guest reality | Mechanism |
|---|---|
| `b` to another function (**tail call**) | `f(ctx); return;` — but via `recomp_continue` |
| crt0's `ldr lr, =exit; bx r3` — run main, *then* return somewhere else | `recomp_continue(ctx, to, ret)` keeps running functions until control is back at our caller |
| a shared epilogue reached with a **stale lr** (ARMCC: 20 branches into one `pop {r4-r11, pc}`) | `recomp_tail_ret()` detects the stale value and tells the callee it returns to *our* return address, via `Ctx.ret_override` |
| falling straight through into the next function (libgcc's `__aeabi_dsub` → `__adddf3`) | treated as a tail call |
| a call the compiler knew never returns (`exit`, `abort`) | `recomp_no_return()` — loud if it does return |
| a return through a register (`pop {r3}; bx r3`) | compared against `ret`, and returns if equal |
| a disguised call (`add lr, pc, #n; b target`) | discovery records where it returns; emitted as a call followed by a `goto` |
| indirect call/jump | `recomp_call` → `recomp_find` (§3.6) |

Every one of these was found by running real code, and each is a patch on the
same assumption. Two things are still outside it: **`setjmp`/`longjmp`** and
**C++ exception unwinding**, both of which return to a point in the *middle* of a
function whose host frame is already gone. Retail games built with ARMCC do use
C++; this is the most likely structural wall ahead (§9.2).

**Lever.** Keep the model; plan for the two holes. A workable design: maintain a
shadow stack of `(guest sp, host jmp_buf)` at function entries that could be
longjmp targets, and implement guest `longjmp` as a host `longjmp` to the frame
whose guest sp matches. The interpreter fallback also covers it, more slowly.

### 3.5 Branches become `goto`, switches become `switch`

**What we do.** An intra-function branch is `goto L_XXXXXXXX;`, and every branch
target gets a label (`L_0010006c: ;` — the `;` because a label may end a block).
Computed jumps become a C `switch` over *every instruction address in the
function*, with `default: recomp_bad_jump(...)`. Jump tables with signed indices
work because the switch is over addresses, not table slots (newlib's
`_vfprintf_r` loads signed halfword offsets and jumps backwards — that was a
real bug).

**Why.** `goto` is exactly the right primitive: clang's optimiser treats the
result as an ordinary CFG and the generated code keeps the original's structure.

**Instead we could have** reconstructed structured control flow (while/if) —
prettier, lossy, and unnecessary.

**What it costs.** The all-addresses `switch` is how a function with a computed
jump gets a jump table with hundreds of cases, and that is exactly what made
clang's `-O2` take ~144 s per example versus ~15 s at `-O0` (§7.7). It is also
conservative: a value that *isn't* one of the addresses aborts, which is right.

**Lever.** Narrow the switch to the targets discovery actually proved
(`_target_switch` already does this where it can). That shrinks the tables and
should get `-O2` back into reach.

### 3.6 Indirect calls go through a sorted table and a binary search

**What we do.** `emit_c.py` emits `by_addr[]`, sorted by entry address with the
Thumb bit in bit 0; `recomp_find_static` binary-searches it;
`recomp_find` tries that, then each attached CRO module's table. A miss is
`recomp_bad_call`, which prints the guest registers and aborts.

**Why.** Simple, compact, and needs no runtime setup. Function pointers,
vtables, callbacks and thread entry points all resolve through one mechanism.

**Instead we could have** used a hash table, a direct-mapped array indexed by
`(addr - text_base) >> 1`, or a perfect hash built at recompile time.

**What it costs.** ~15-20 dependent loads per indirect call for a game with
30k functions. C++ code (vtables everywhere) calls indirectly constantly. The
module walk adds to it.

**Lever.** A flat array of `GuestFn` indexed by halfword offset from the text
base: for 8 MB of code that is 4 M entries × 8 bytes = 32 MB — too much. Two
levels (a 4 KB-page table of small arrays) gets it to ~1-2 MB and makes lookup
two loads and no branches. Or keep binary search but build a perfect hash
offline. Easy, isolated, measurable win.

### 3.7 There is no guest PC

**What we do.** `Ctx.r[15]` exists but is never maintained. Reading pc is folded
to a constant at translate time (ARM: instruction + 8, Thumb: + 4), because the
value is known statically.

**Why.** Maintaining it would mean a store per instruction, for nothing.

**What it costs.** No precise guest-fault reporting, no "where am I" at an
arbitrary point, and no interpreter hand-off *mid-instruction*. For fault
reporting the error paths pass the address explicitly (that is why every trap
helper takes an `addr`).

**Lever.** If you add the interpreter, the hand-off points are function
boundaries and jump targets, where the address *is* known — so this stays fine.

### 3.8 Time passes on back edges

**What we do.** Every backward branch emits `RECOMP_BACKEDGE()`, which
decrements `ctx->budget`; when it hits zero, `recomp_tick` calls the OS layer's
tick handler (advance the virtual clock by 20 µs, fire due timers, maybe switch
threads). `RECOMP_TICK_EDGES` is 1024.

**Why.** The OS layer only gets control inside an svc. A guest spin loop (waiting
on a flag another thread sets, or on a timer) would otherwise freeze virtual time
and deadlock forever. Counting back edges is deterministic — it counts, it does
not time.

**Instead we could have** counted instructions (more accurate, needed for real
cycle accounting, costs more), used host wall-clock time (nondeterministic, and
determinism is the basis of the whole test strategy), or used host signals
(hostile to iOS).

**What it costs.** One decrement and a branch per loop iteration, and virtual
time that is only loosely related to instructions executed. Games that measure
their own frame time will see something odd.

**Lever.** When you get to proper timing, the natural upgrade is: have the
translator count instructions per basic block at compile time and add that
constant to a cycle counter at block entry (one add per block, not per
instruction) — that is the "cycle counting" item on the roadmap, and it is what
timers, DSP and vblank pacing will eventually need.

### 3.9 VFP: raw bits in, raw bits out

**What we do.** The VFP file is a union of `u[32]`/`x[16]` (raw) and
`f[32]`/`d[16]` (values). All arithmetic goes through `runtime/vfp.h`, which does
the maths in plain C but handles everything around it explicitly: NaN
propagation (SNaN first, then operand order, quieted on the way out), ARM's
default NaN `0x7FC00000` (x86 produces `0xFFC00000`), saturating float→int
conversion (a plain C cast is undefined for out-of-range values), non-fused
multiply-accumulate, `vneg`/`vabs` as sign-bit operations even on NaNs, and the
two FPSCR mode bits libctru sets on every thread (FZ flush-to-zero, DN default
NaN).

**Why.** Ordinary IEEE arithmetic is identical on host and guest, so only the
edges need modelling — and those edges differ between ARM and x86 in ways that
show up as wrong pixels, not crashes.

**Instead we could have** used a soft-float library (always exact, ~50x slower),
or just used C's `float` and hoped (wrong NaNs, undefined conversions).

**What it costs.** Explicit bit-twiddling around every operation, and
unmodelled modes are hard errors (`recomp_bad_fpscr`): rounding modes other than
nearest, short vectors, trapped exceptions, cumulative exception flags.

**Lever.** Right call. When you hit a game that sets a rounding mode, the fix is
per-operation `fesetround`, which is slow — better to implement it only for the
instructions that need it.

### 3.10 The exclusive monitor is a pair of fields

**What we do.** `ldrex` records the address and arms a flag; `strex` succeeds only
if the flag is still armed for the same address; a context switch clears it.

**Why.** Only one guest thread runs at a time (§5.2), so the monitor reduces to
exactly this. Real atomics would be needed only with real parallelism.

**Lever.** If you ever run guest threads truly in parallel (§5.2's lever), this
becomes a genuine compare-and-swap problem, and `ldrex`/`strex` would need host
atomics plus a per-address monitor. It is one of the hidden costs of that idea.

---

## 4. Layer 3: the OS layer (`hle/`)

### 4.1 High-level emulation, written from public docs

**What we do.** `svc #n` becomes a call to `recomp_svc_handler`, and `hle/`
implements the 3DS kernel (Horizon) and its services in C: threads, sync
objects, IPC, and one file per service (`srv`, `apt`, `gsp`, `hid`, `fs`, `cfg`,
`dsp`, `ldr:ro`, `y2r`, plus `stubs.c` for services that only need to say
"fine"). Everything is written from 3dbrew and the libctru/citro3d headers.

**Why.** Three reasons, in order of importance:

1. **Licence freedom.** The original plan was to reuse Azahar's HLE and GPU,
   which would make this project GPLv2. Writing our own keeps the licence open.
2. HLE is the only option anyway without the user's firmware and keys.
3. It is far easier to debug a service you wrote than a kernel you emulate.

**Instead we could have** reused Azahar's services and video core (much faster
progress, GPLv2, and a dependency on their internals), or emulated the real
kernel at the ARM level (LLE — needs firmware, and would have to emulate the
hardware the kernel talks to).

**What it costs.** The service surface is effectively unbounded — that is what
the grey in `docs/progress.svg` is. Every game wants a slightly different set,
and a retail game wants many more than homebrew (that is why `ndm:u`, `ptm:u`,
`y2r:u`, `ldr:ro`, ExtSaveData all appeared while getting Pokémon X to boot).

**Lever.** This is a volume problem, not a design problem, and the design is
right. The accelerator is the progress map plus `HLE_LENIENT`/`HLE_TRACE`: run
the game, see what it asks for, implement that. Keep `stubs.c` growing — many
services genuinely only need "fine".

### 4.2 One host thread per guest thread, one running at a time

**What we do.** Each guest thread gets a real pthread with a 16 MB host stack,
but a single `big_lock` means exactly one runs at a time. The running thread
gives up the CPU **only inside an svc** (block, yield, or waking a
higher-priority thread) or on a back-edge tick (§3.8). Scheduling is strict
priority, round-robin within a priority by `ready_seq`. Switching is a
`pthread_cond_signal` to the next thread and a `pthread_cond_wait` on your own.

**Why.** A guest thread is a deep stack of recompiled C calls, so it *needs* a
real host stack — you cannot switch away from the middle of `f_00123456` without
one. And making it cooperative makes every run **bit-for-bit deterministic**,
which is what lets the entire test strategy be "compare against a golden
log/hash".

**Instead we could have:**

- **Real preemptive host threads** with fine-grained locking: uses multiple
  cores, matches hardware, and destroys determinism — plus every piece of shared
  guest state becomes a data race, and the exclusive monitor (§3.10) becomes
  real work.
- **Fibers / `ucontext` / hand-rolled stack switching**: same determinism, much
  cheaper switches (a register save, no kernel involvement), no pthread
  dependency. Portable-ish, and `swapcontext` is deprecated on Apple platforms
  but easy to write by hand in asm.
- **A state machine with no host stack** (what a block-dispatch design allows):
  cheapest switching of all, and incompatible with §2.4.

**What it costs.** A thread switch is two condvar operations — microseconds, not
nanoseconds. 16 MB × up to 32 threads of host stack. And no use of the host's
other cores, ever, for guest code. Also, the 3DS has 2 (or 4) ARM11 cores and
games do place threads on them; we ignore cores entirely, so a game that depends
on real parallel progress between two threads could behave differently.

**Lever.** Keep cooperative scheduling — determinism is worth more than
parallelism here, and the 3DS's own scheduler is mostly cooperative in practice.
If switch cost shows up in a profile, replace pthreads+condvars with hand-rolled
fibers; it is a contained change behind `switch_to`.

### 4.3 A virtual clock

**What we do.** Time is a tick counter (`TICKS_PER_SEC = 268111856`, the real
ARM11 rate). Each svc costs 1 µs; each back-edge tick costs 20 µs. When every
thread is blocked, `idle_advance` jumps the clock straight to the next deadline
(a sleep, a wait timeout, or the 60 Hz vblank). If nothing can ever happen, it
prints a **deadlock report** naming each thread and what it waits on, and exits
with status 4. A virtual-time limit catches hangs.

**Why.** Deterministic, fast (idle time costs nothing), and the deadlock report
turns the most common class of HLE bug — "the app is waiting for something we
never signal" — from a hang into a named diagnosis. That report is one of the
best things in the codebase.

**Instead we could have** used real wall-clock time (what an emulator does;
needed eventually for playable framerate and audio), or counted instructions for
a cycle-accurate clock (§3.8's lever).

**What it costs.** Virtual time has no relation to real time, so the current
runner is a batch tool, not a playable emulator. Anything that depends on the
*ratio* of CPU speed to frame rate will be off.

**Lever.** When you want interactive play, the clock becomes a hybrid: free-run
virtual time, but pace vblank against real time and let audio drive it. Keep a
`--deterministic` mode for the tests — do not lose it.

### 4.4 Flat tables for objects and handles

**What we do.** `KObj` is one struct with a type tag and a union per type (event,
mutex, semaphore, thread, arbiter, shared memory, session). Handles are slots in
a 4096-entry table, numbered `0x100 + index`, handed out by a cursor that does
not reuse a slot until it wraps (so a stale handle stays invalid for a long
time). Every object ever created is kept on an `all_objs` list and never freed.

**Why.** It is the simplest thing that behaves like Horizon's handle table, and
the no-reuse cursor turns use-after-close bugs into clean "bad handle" errors
instead of silent aliasing. Never freeing is a deliberate simplification.

**Instead we could have** used refcounted objects with proper close semantics
(correct, and needed if a long session creates objects in a loop).

**What it costs.** Leaks, bounded only by the run's length — fine for minutes,
not for hours of gameplay. 4096 handles and 32 threads are hard limits.

**Lever.** When runs get long, add refcounting (`KObj.refs`, incremented per
handle) and free on zero. Cheap to do, and the `all_objs` list already gives you
the audit trail.

### 4.5 IPC is a command buffer and a dispatch function per service

**What we do.** An IPC request is words in the calling thread's TLS at `+0x80`;
word 0 is the header (`command id << 16 | normal << 6 | translate`). Each
service is a `{ name, handle }` pair, and `handle(session, cmd)` switches on the
command id and writes the reply over the request. Per-session state lives in
`session->data`, which is how an open file or directory becomes "a session
served by `fs_file_handle`".

**Why.** It mirrors how the console actually works, which means the guest's own
libctru code works unmodified, and it keeps each service file independent.
Reusing sessions for open files is a nice trick: everything is one mechanism.

**Instead we could have** intercepted libctru's functions by name instead of
implementing IPC (far less work, and it only ever works for homebrew built
against the libctru we know — retail games call the services directly).

**What it costs.** You implement the wire format, including the translate
descriptors (handles copied/moved, buffers mapped/copied). Worth it.

### 4.6 Unknown anything is a hard stop that names itself

**What we do.** An unimplemented IPC command → `ipc_unhandled` prints the
service and command and stops. An unimplemented svc → prints the number and
r0-r3. A GPU feature we do not have → `gpu_fatal` names it. An indirect call to
code we do not have → prints every register. No thread can run → the deadlock
report. `HLE_LENIENT=1` replies bare success instead, for exploring;
`HLE_TRACE=1` logs every svc and IPC; `GPU_TRACE=1` names every register write.

**Why.** It is the same rule as §2.7, applied to the OS, and it is the reason
progress on this project is steady rather than mysterious: at any moment there
is a *named* next thing to implement. Emulators that log-and-continue get more
software to boot and then spend months on "it renders wrong somewhere".

**What it costs.** One missing command stops an app that might otherwise have
limped on. `HLE_LENIENT` is the escape valve, and the compat sweep's
classification (`stops` with the reason) turns every stop into a line in a
table instead of a mystery.

**Lever.** Keep this rule above all the others. It is the project's actual
methodology.

### 4.7 Memory is already there; the OS just bookkeeps

**What we do.** The whole 4 GiB is backed, so `svcControlMemory` ALLOC does not
allocate anything — it hands out addresses and records a region in a sorted
`regions[]` table with state and permissions. `svcQueryMemory` answers from that
table (libctru uses it to find room for shared memory). The linear/FCRAM heap
hands out addresses from `0x14000000` as on a real console.

**Why.** Allocation is free when the address space is flat (§3.2), and the only
thing the guest can actually *observe* is the answer to QueryMemory.

**What it costs.** No protection (a guest write to unallocated memory succeeds),
`MEMOP_PROTECT` is a no-op, and MAP/UNMAP mirroring is recorded rather than
real — so a game that writes through one mapping and reads through its mirror
would not see it. (Today it mostly does not matter; if it starts to, the fix is
to make MAP actually `mremap`/`mmap` the same pages at both addresses, which the
flat mapping makes possible.)

### 4.8 `ldr:ro` loads modules whose code was compiled months ago

**What we do.** The game reads a `.cro` into its heap and asks `ldr:ro` to load
it. We do what RO does — turn header offsets into addresses, place the segments,
apply internal patches, resolve imports against `static.crs` and other loaded
modules, back-patch earlier modules that import from this one — and then attach
the matching recompiled module (§2.8) at that base. A module with no recompiled
code is a hard stop naming it.

**Why.** It is the only way AOT translation and run-time module loading coexist.

**What it costs.** A full CRO relocator in C, mirroring the one in Python. The
two must agree about what gets patched; `cro.without_patched()` is the shared
assumption.

---

## 5. Layer 4: the GPU (`gpu/`)

### 5.1 Our own software PICA200

**What we do.** `gpu/` is a complete software implementation of the 3DS GPU from
public docs: the 2D engines (`gx.c`: memory fill, display transfer with tiling
and format conversion, texture copy), command-list decoding (`regs.c`), a vertex
and geometry shader interpreter (`shader.c`), the full fixed-function pipeline
(`draw.c`: vertex fetch, clipping, rasterisation, the six-stage texture
combiner, alpha/stencil/depth tests, blending, logic ops), textures (`texture.c`:
14 formats including ETC1, filtering, mipmaps, cube maps, the procedural unit)
and fragment lighting (`lighting.c`).

**Why.** The licence (§4.1), and because a software rasteriser is *checkable*:
every pixel is deterministic, so a frame hash is a valid test.

**Instead we could have:**

- **Reused Azahar's video core**: months saved, GPLv2, and a dependency on a
  codebase designed around a different CPU backend. This was the explicit
  decision point at M8, and you chose to write it.
- **Translated PICA state to a host GPU API** (Vulkan/Metal/OpenGL): the only
  way this is playable on a phone, and *much* harder to make pixel-exact. Note
  one important fact for the iOS plan: compiling *shaders* at run time is
  allowed on iOS (Metal's compiler is available to apps), even though JITting
  CPU code is not. So the GPU layer does not have to be AOT.
- **Software, but SIMD and multithreaded**: the obvious middle step.

**What it costs.** Speed. A software rasteriser at 400×240×2 screens, 60 fps,
with per-pixel lighting, is far from real time, and the GPU will be the
bottleneck long before the recompiled CPU code is.

**Lever.** The architecture to aim for: keep this software path as the
*reference* (it is a correctness oracle you cannot buy), and add a hardware
backend behind the same interface. Then any disagreement between them is a bug
you can localise. That is a much stronger position than having only a fast
renderer.

### 5.2 Synchronous and in-order

**What we do.** GSP drains the GX queue when the app triggers it; a command list
runs to completion inside that call; `draw` rasterises immediately. Interrupts
(PSC0/PSC1/PPF/P3D/DMA, VBlank0/1) are raised at the points the hardware raises
them.

**Why.** Determinism again, and it removes a whole class of synchronisation bug.

**What it costs.** No overlap between CPU and GPU work, so no way to hide GPU
latency, and a game that depends on the GPU being *slow* (racing the beam,
reading a buffer before the GPU finished) sees different timing.

**Lever.** If you later want a real GPU thread, do it behind the GX queue — that
is already the natural boundary.

### 5.3 Shaders are interpreted, in float32

**What we do.** `shader.c` interprets the full PICA vertex/geometry ISA
including swizzles, masks, relative addressing, the call/if/loop stacks, and
bool/int uniforms, with the hardware's quirks (`inf * 0 = 0`, no negative zero,
NaN-aware min/max). Arithmetic is float32; the hardware's float24 precision is
not modelled.

**Why.** Shaders are uploaded at run time, so AOT translation is impossible by
definition, and interpreting is the correct baseline. float32 is a deliberate
simplification: it is *more* precise than the hardware, which mostly shows up as
sub-pixel differences.

**Instead we could have** implemented exact float24 (truncate every result
through `f32_to_f24` — slower but bit-exact), or translated shaders to host
shaders at run time (which, per §5.1, is legal even on iOS).

**What it costs.** Geometry that depends on float24 rounding (coplanar surfaces,
exact edge tests) can differ, which can show as z-fighting or seams.

**Lever.** Exact float24 is a contained change (one rounding function applied at
each operation's output in the interpreter) and worth doing when a game shows
precision artefacts. Keep it behind a flag so you can A/B frames.

### 5.4 Undocumented features stop by name

**What we do.** Procedural-texture noise and coordinate shifts are not publicly
documented, so they stop with a message naming them rather than guessing — and
rather than reading Azahar's implementation, which would taint the licence.

**Why.** Licence hygiene, and honesty in the compat table (two examples are
listed as stopping for exactly this reason).

**Lever.** These are reverse-engineerable from hardware output if you ever have
the hardware and the patience; or they are the specific places where you might
decide the GPLv2 trade is worth it. A conscious, isolated decision — which is
exactly how you want that kind of thing to be structured.

---

## 6. Layer 5: testing and tooling

This is the part of the project most worth being proud of, and the reason the
rest works. Five independent kinds of test, each catching what the others miss.

### 6.1 Instruction-by-instruction differential testing against Unicorn (M3)

**What we do.** `python -m recomp --trace` emits `RECOMP_TRACE(addr, "disasm")`
before every instruction and reports every memory write. `tests/m3/diff.c` runs
each function twice — once in Unicorn 2.0.1 (CPU model ARM11MPCore, flat 4 GiB
map) on the *original bytes*, once in our recompiled C — over many random
inputs, and compares r0-r14, NZCV, d0-d15, the FPSCR flags and every memory
write **before every instruction**. The first divergence is reported with the
instruction that caused it. `tests/m3/selftest.sh` plants a bug and checks it
gets pinpointed.

**Why.** It is the difference between "my tests pass" and "my CPU is correct".
It found: Capstone's wrong `update_flags` for adc/sbc/rsc, the missing
address-space wraparound, and (per the milestone notes) a good share of
everything since.

**Instead we could have** compared only at function exit (cheaper, and a
divergence tells you nothing about where), compared against hardware traces
(the gold standard, needs hardware), or trusted hand-written unit tests.

**What it costs.** A Unicorn dependency for tests, a skip list for functions
that take function pointers (random inputs would make them call random
addresses), and runtime.

**Lever.** The one thing missing is a **fuzzer**: generate random *encodings*
(all condition codes, all shift forms, all register combinations) and diff them
against Unicorn, rather than only testing instructions that appear in programs
you compiled. That would have found the Capstone bugs before real code did, and
it is maybe 150 lines on top of the existing harness. Highest-value test work
available.

### 6.2 Differential testing against natively compiled C

**What we do.** Milestone harnesses compile the *same* C source twice — once for
ARM (then recompiled) and once for the host — and compare results over edge
cases and random inputs.

**Why.** It checks semantics end to end, including the ABI, cheaply.

**What it costs.** Only tests what C generates; corners like `lsr #32`, `rrx`
and exact flag behaviour need hand-written asm, which is why `tests/m1/flags.S`,
`tests/m2/addr.S`, `tests/m4/thumb.S`, `tests/m5/vfp.S`, `tests/real/dsp.S`,
`tests/real/tables.S` exist.

### 6.3 Negative tests

**What we do.** `tests/expect_reject.sh` asserts the recompiler *refuses* bad
input with the right message: unsupported instructions, unlinked objects with
relocations, mode-mixing, `vcvtr`.

**Why.** §2.7's rule is only real if it is tested. This is the test that stops
"it got quietly skipped" from ever becoming possible again.

### 6.4 Whole programs: golden logs and frame hashes

**What we do.** `tests/m6/threads.c` checks the kernel against a golden
`threads.expected` log; `deadlock.c` checks the deadlock report; the hello world
must produce a frame whose SHA-256 matches `hello_frame.sha256`.

**Why.** Only a whole program exercises the interaction of CPU, kernel, services
and GPU. Hashing the *pixels* is possible only because everything is
deterministic (§4.2, §4.3, §5.2) — determinism is not a nicety here, it is what
makes the test strategy exist.

**What it costs.** A hash is all-or-nothing: an intentional one-pixel change
breaks every hash, hence `--record`. Review the frames when you re-record;
nothing else will catch a wrong-but-stable picture.

### 6.5 The compatibility sweep as the project's metric

**What we do.** `make compat` copies all 62 of devkitPro's 3ds-examples, builds
each with its own Makefile, recompiles it, links it against the OS layer, runs it
with scripted input, classifies the outcome on a ranked scale
(`build-failed < recomp-failed < c-failed < stops < runs`), compares against the
expected outcome in `examples.txt` and the frame hash in `hashes.txt`, and
writes `docs/compat.md`. It fails if anything *regresses*; `--record` accepts
improvements. The OS layer is built once into `liboslayer.a` and unchanged steps
are skipped — a full sweep is ~3.6 min, ~9 s when nothing changed.

**Why.** It is an honest, external, hard-to-fool number ("49 run"), and the
ranked expectations mean you cannot accidentally go backwards.

**Instead we could have** curated a smaller suite (faster, less honest) or used
retail games as the metric (not committable, not shareable, not reproducible).

**What it costs.** Depends on devkitPro's examples being installed, and 8 of
them do not build for reasons that have nothing to do with us.

**Lever.** This is the right metric and the right harness. The missing piece is
that **nothing runs it automatically** — see §10.8.

### 6.6 Tracing and introspection

`--trace` (lockstep), `HLE_TRACE` (every svc and IPC), `GPU_TRACE` (every
register write, with names generated by `tools/gen_regnames.py`), the deadlock
report, `--keep-going` with a grouped failure report, `OUTPUT.discovery.txt`
(what discovery found and what it rejected), `make progress` (a treemap of every
documented service command, every ARMv6K/VFPv2 instruction and every PICA shader
instruction, green where implemented), and the live view (`--live` writes both
screens to a BMP every 30 frames, written to a side file and renamed so a viewer
never sees half a picture; `tools/live.html` + `live_server.py` show it and feed
input back). All of it opt-in and zero-cost when off.

This is unusually good, and it is why a one-person project can carry four
subsystems at once.

### 6.7 The build system

**What we do.** One 564-line Makefile with a target per milestone, ARM flags
(`-marm -mcpu=mpcore -O1`, linked at `0x00100000`, `-nostdlib`), host flags
(`-std=c11 -O1 -g -Wall -Wextra -Werror -fno-math-errno
-fsanitize=undefined,address -fno-sanitize-recover=all`), and `.SECONDARY` so
intermediate `.elf`/`.c` files survive for inspection.

**Why.** `make test` is the whole gate, warnings are errors, and sanitizers are
on everywhere — so a mistranslation that corrupts memory is caught at the moment
it happens rather than later.

**Notable sub-decisions:**

- **Generated C is compiled `-O0`** in the sweep, because `-O2` took ~144 s per
  example against ~15 s — a direct consequence of the giant switches (§3.5). So
  the project's performance claim is currently *untested*: the fast path is
  never built with optimisation.
- **`--split N`** puts functions in chunks of N files so a big program compiles
  in parallel.
- **`liboslayer.a`** builds the OS layer once for the whole sweep.

**Lever.** Two things. (1) Fix the cause of the `-O2` cost (narrow the switches)
and then measure an optimised build — until then nobody knows how fast this is.
(2) The Makefile is near the limit of what is pleasant to extend; if you add
much more, a generated build (CMake/ninja) would pay for itself. Not urgent.

---

## 7. The legal architecture

Worth stating as a design constraint, because it has shaped the code more than
any technical consideration:

- **No ROMs, dumps, keys, firmware, or recompiled commercial output in git.**
  `dumps/` and `out/` are gitignored; `tools/build_dump.sh` refuses an output
  directory outside `out/`.
- **We never handle keys.** `ctr_loader.py` reads only decrypted images
  (checking the NCCH NoCrypto flag) and says so in the error message.
- **Metadata is fine**: compatibility tables, per-game addresses, hashes.
- **Firmware data is never shipped**: the system font comes from the user's own
  dump via `--shared-font`, which is why 3 examples legitimately stop.
- **No Azahar code**, which is what keeps the licence open — and is why `gpu/`
  and `hle/` exist at all, and why undocumented features stop by name (§5.4).

**Open decision: there is no licence.** The README says "not chosen yet". That
was correct while reusing Azahar was still on the table; now that `gpu/` and
`hle/` are your own, it is a decision you can make — and the longer it waits,
the more contributors and code exist under no terms at all. Pick one (MIT,
Apache-2.0, or GPLv2 if you still want to leave the Azahar door open).

---

## 8. Where the design will bite next

In the order I would expect to meet them.

### 8.1 Distribution on iOS is the unsolved part of the plan

The technical plan (AOT translation) is sound. The product plan is not finished,
and it is worth thinking through *now* because it may influence the
architecture:

- The output is C that must be compiled and linked — so a build happens on a
  computer, not the phone.
- You cannot distribute a recompiled commercial game. So the only lawful shape
  is: the user supplies their own dump, the toolchain runs on their machine, and
  the result is sideloaded (Theos + SideStore, as planned).
- That means **one app per game**, with a per-game build of several minutes to
  hours, and an IPA whose size is driven by generated code.
- Apple's 2024 rules allow retro-game *emulators* on the App Store, but not JIT.
  An interpreter-based app is distributable; an AOT-per-game app is not.

None of this makes the project pointless — it is a genuinely interesting
engineering result either way, and "fast 3DS on iPhone for your own dumps" is a
real thing people would use. But if a shippable app is the goal, the realistic
architecture is a **hybrid**: ship an interpreter-based runtime, and let the AOT
path accelerate what it can. Which, conveniently, is the same thing §10.4 asks
for.

### 8.2 C++ exceptions and `setjmp`/`longjmp`

Retail games are ARMCC-built C++. Exception unwinding returns into the middle of
a function whose host frame no longer exists, which the mirrored-stack model
(§3.4) cannot express. Expect this the moment a game takes an error path. Plan:
the shadow-stack design in §3.4's lever, or the interpreter.

### 8.3 Code size and compile time

The hello world is 303 functions → ~47k lines of C. A retail game is tens of
thousands of functions. Extrapolating, expect millions of lines, a long
compile even split and parallel, and a binary several times the size of the
original ARM code. There are good answers (compile cold functions `-Oz`, hot
ones `-O2`; narrow the switches; split more) but you need the numbers first.

### 8.4 Performance is still unmeasured

The project exists to be fast, and there is no benchmark. The generated code is
currently built `-O0` in the sweep. Everything in §3 about register
localisation and flag liveness is reasoning, not measurement. This is the
cheapest gap to close and it gates every other performance decision.

### 8.5 Volume in the OS layer

Every new game wants more services. This is expected and the design handles it;
just do not be surprised when "boot one more game" is three weeks of FS and APT
edge cases rather than anything architectural.

### 8.6 Single-threaded by construction

Guest code will never use more than one host core (§4.2). On a phone, with a
software GPU (§5.1) competing for the same core, that is a hard ceiling. The GPU
is the one that can be parallelised without giving up determinism of the *CPU*
side — do that before you consider parallel guest threads.

---

## 9. How to make it better, in order

Ranked by (value ÷ effort), with what each depends on.

### 9.1 Build a benchmark. First.

`make bench`: a fixed workload (say, the hello world for N frames, plus a
compute-heavy homebrew), built `-O2`, reporting guest instructions per second
and wall-clock per frame, with a committed baseline. Also measure against
Azahar's interpreter if you can, since beating it is the premise.

*Effort:* a day. *Value:* it turns every item below from an opinion into a
measurement. Nothing else in this list should happen first.

### 9.2 Narrow the computed-jump switches

§3.5: emit `case` labels only for the targets discovery proved, not every
instruction address in the function. This is probably what makes clang's `-O2`
cost 10x, which is what forces `-O0`, which is what hides the project's actual
speed. `_target_switch` already does the right thing where targets are known —
extend it, and make "no proven targets" the exceptional path.

*Effort:* small, localised in `translate.py`. *Value:* potentially unlocks
optimised builds entirely.

### 9.3 Introduce an IR between decode and emit

§2.3. A dataclass per operation with explicit `reads`/`writes`/`flags` and a
`to_c()`. Nothing else changes on day one.

*Effort:* a week, mechanical, and the Unicorn lockstep harness makes it safe —
you can refactor with a correctness oracle running.
*Value:* it is the prerequisite for 9.4, 9.5 and any future LLVM backend, and it
makes `translate.py`'s 1700 lines maintainable.

### 9.4 Register localisation

§3.1. Per function: compute the set of guest registers used, hold them in C
locals, flush and reload at calls / svcs / traps / returns / indirect jumps.
Expect this to be the largest single speedup, because it removes a memory
round-trip per instruction *and* removes the aliasing problem with `ctx->mem`.

*Depends on:* 9.3, and 9.1 to prove it. *Effort:* medium. The flush points are
exactly the places §3.4 already enumerates, which is a good sign.

### 9.5 Dead-flag elimination

§3.3. A backwards pass per function: if no reachable instruction reads a flag
before overwriting it, do not compute it. ARM sets flags everywhere and reads
them rarely, so this should remove a large fraction of all flag work.

*Depends on:* 9.3. *Effort:* medium; the analysis is textbook liveness.

### 9.6 An interpreter fallback

§0, §2.7, §3.4, §8.1, §8.2. ~1-2k lines of C reusing every helper in
`context.h`/`vfp.h`, dispatching on the raw encoding. Wire it in at
`recomp_bad_call`, `recomp_bad_jump`, `recomp_bad_return` and `--keep-going`
stubs: instead of aborting, interpret from that address until control reaches a
recompiled entry point, and *log it* so it still shows up as work to do.

*Value:* this one change converts the project's three structural limits (§0)
from walls into slow paths. It makes retail games boot further, covers C++
exceptions and computed jumps, makes `--keep-going` builds genuinely runnable,
and it is the piece a distributable iOS app would need anyway. It is also legal
on iOS, which is the whole reason it works.
*Effort:* real but bounded, and the Unicorn harness tests it for free.

### 9.7 O(1) indirect-call lookup

§3.6. Two-level table (page → array of `GuestFn`) keyed on the halfword offset
from the text base. Measure first — if a game is vtable-heavy, this is free
speed.

*Effort:* an afternoon in `emit_c.py` + `runtime.c`.

### 9.8 An instruction fuzzer

§6.1. Random encodings → diff against Unicorn. Catches whole classes of
Capstone-trust and flag-semantics bugs that real programs only reach by luck.

*Effort:* ~150 lines on the existing harness. *Value:* high, permanently.

### 9.9 CI

There is no `.github/` at all. `make test` is a strong gate that currently only
runs when you remember. devkitPro publish Docker images, so an Actions workflow
running `make test` (and the sweep, nightly) is very doable.

*Effort:* a day. *Value:* the gate becomes real.

### 9.10 Write the decoder (optional)

§2.2. Replace Capstone's semantics with your own ~600-line ARMv6K decoder, keep
Capstone for comment text. Removes every workaround and the version pin.

*Effort:* medium, well-tested by 9.8. *Value:* correctness confidence and the
end of fighting a disassembler for semantic facts.

### 9.11 Choose a licence

§7. It is a one-line change and a real decision; the cost of deferring grows.

### 9.12 Plan the hardware GPU backend as a second implementation

§5.1. Keep the software rasteriser as the reference oracle; add a Metal/Vulkan
backend behind the same interface; diff frames between them. Remember that
run-time *shader* compilation is allowed on iOS even though CPU JIT is not — so
the GPU side does not have to be AOT.

*Effort:* large. *Value:* it is the difference between "correct" and "playable".
Do it after the CPU side is measured and the interpreter exists.

---

## 10. Quick reference

| # | Decision | Chief alternative | Why ours | Main cost |
|---|---|---|---|---|
| 0 | AOT recompile to C | interpreter / JIT | iOS forbids JIT; native speed | must find all code statically; per-game builds |
| 2.1 | Python translator | C++/Rust | offline, easy to rewrite | can't run on device |
| 2.2 | Capstone decoder | own decoder | free, correct-ish, gives comments | wrong semantics in places; we re-read encodings |
| 2.3 | no IR, emit C text | IR + backends | shortest path, readable output | no liveness, no register allocation, one backend |
| 2.4 | one C fn per guest fn | per-block + dispatch | native calls, optimisable, debuggable | exotic control flow needs special cases |
| 2.5 | static discovery + verify | trace-driven | works on symbol-less retail code | heuristic; invisible code is absent |
| 2.7 | unsupported = hard error | no-op / interpret | bugs stay findable | coverage limited by instruction set |
| 2.8 | relocatable CRO output (`RB`) | fixed address / JIT | AOT survives runtime loading | add per code address; modules fixed at build |
| 3.1 | all state in `Ctx *` | ABI recovery | uniform, safe, testable | memory round-trip per instruction |
| 3.2 | flat 4 GiB mmap | software MMU | one host load per guest load | no protection, no MMIO traps |
| 3.3 | eager flags, 4 words | lazy flags | correct by construction | stores for flags nobody reads |
| 3.4 | host stack mirrors guest | PC in a variable | native call/ret | longjmp/exceptions don't fit |
| 3.5 | `goto` + big `switch` | structured recovery | exact CFG | huge switch tables; clang `-O2` cost |
| 3.6 | sorted table + bsearch | hash / direct table | simple, compact | ~15 loads per indirect call |
| 3.8 | back-edge budget | instruction count / real time | deterministic, cheap | loose relation to real time |
| 3.9 | raw-bit VFP + ARM rules | soft-float / plain C | exact where it differs, fast where it doesn't | bit-twiddling; some modes unimplemented |
| 4.1 | own HLE from docs | reuse Azahar / LLE | licence stays open | unbounded service surface |
| 4.2 | pthread per guest thread, one at a time | preemptive / fibers | needs host stacks; fully deterministic | µs switches; one core only |
| 4.3 | virtual clock | wall clock | deterministic; deadlock reports | not playable in real time |
| 4.6 | unknown = named stop | log and continue | always a named next task | one gap stops the app |
| 5.1 | own software GPU | Azahar's / host GPU | licence; pixel-exact oracle | far from real time |
| 5.3 | interpret shaders, float32 | exact float24 / host shaders | shaders only exist at run time | precision artefacts possible |
| 6.1 | Unicorn lockstep diff | exit-state compare | pinpoints the exact instruction | test-only dependency |
| 6.5 | compat sweep as metric | curated suite | honest, external, regression-proof | needs devkitPro examples |
