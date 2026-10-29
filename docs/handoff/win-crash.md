# Brief: a crash in the native Windows build

Read `CLAUDE.md` first (the legal rules in particular). Game files, recompiled
output and anything you learn about the game's code stay in `out/` and are
never committed. Don't push; report back with a diff and an explanation.

## The problem

The native Windows build of the owner's Pokémon X replay crashes at about
frame 41,600, on the way to the starter Pokémon choice. The Linux build
runs the same replay past that point, so it is something that differs on
Windows.

The crash report (`hle: crash: ...`, from `on_crash` in `tests/m6/runner.c`)
says:

```
exception c0000005 at image base + 0x5e07dc4   -> pica_raster_flush + 0x274 (gpu/draw.c)
writing host 0x00000219a7cfd808                -> a heap address, NOT guest memory
```

`raster_band` is inlined into `pica_raster_flush`. Drawing is switched off at
that point (`GPU_SKIP_UNTIL=42200` fast-forwards without drawing), so
questions to answer:

- Why are triangles queued at all while `gpu_skip_draws` is set? (Check every
  path into `raster_queue`.)
- Which write faults: the target colour/depth buffer pointer in a queued
  triangle's `Target`, the per-band stats, or something else?
- What differs on Windows: `long` is 32-bit (LLP64), winpthreads, the thread
  pool in `gpu/draw.c` (`pool_start`, `pool_run`, `band_stats`), `realloc` of
  the queue while workers might still read it, alignment, `-O1` codegen.

## How to reproduce (about 4 minutes)

Build inside WSL (from Git Bash, prefix `MSYS_NO_PATHCONV=1`):

```bash
wsl -d Ubuntu-24.04 -u root --cd /mnt/c/any3ds -- bash -lc 'OS_ONLY=1 tools/build_win.sh out/game'
```

`OS_ONLY=1` relinks only the OS layer (`hle/`, `gpu/`, `runtime/`, the runner):
seconds. To get line numbers, add `-g` to `OS_FLAGS` in `tools/build_win.sh`
temporarily. The linker map is `out/game/win/run.map` (the image base is
0x140000000).

Run on Windows, from `C:\any3ds\out\game`, in Git Bash:

```bash
GPU_SKIP_UNTIL=42200 ./win/run.exe --romfs "C:/3DS DUMPS/Pokemon X.3ds" --shared-font "C:/3DS DUMPS/cbf_std.bcfnt" --save save --max-seconds 720 $(cat inputs_now4.args) > win_crash.out 2>&1
grep crash win_crash.out
```

Use a copy of the `save` folder if you change anything that could write to it.
The run is deterministic: same inputs, same crash.

To check the Linux build passes the same point (about 8 minutes the first
time; `out/game/run` is the Linux binary):

```bash
wsl -d Ubuntu-24.04 -u root --cd /mnt/c/any3ds/out/game -- bash -lc 'GPU_SKIP_UNTIL=42200 ./run --romfs "/mnt/c/3DS DUMPS/Pokemon X.3ds" --shared-font "/mnt/c/3DS DUMPS/cbf_std.bcfnt" --save save --max-seconds 720 $(cat inputs_now4.args) > linux_run.out 2>&1'
```

## Done when

- The Windows replay reaches frame 42,264 and draws (`gpu stats` lines show
  triangles after frame 42,200).
- `make test` still passes in WSL.
- You report: the root cause, the diff, and how you checked it. Temporary
  debugging code is removed.
