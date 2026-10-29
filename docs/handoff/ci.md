# Hand-off: continuous integration for any3ds

A self-contained task for another assistant. Read `CLAUDE.md` first (the
project's rules), then this.

## Goal

Every push and pull request to `main` runs the public test suite
(`make test`) on GitHub Actions and shows pass/fail. The suite needs **no
game data**: it builds its own test programs with devkitPro and runs them.

## What `make test` needs

Measured on the maintainer's machine (WSL2, Ubuntu 24.04), where it passes:

| Tool | Version there | What uses it |
|---|---|---|
| clang | 18.1.3 (Ubuntu 24.04's `clang`) | builds harnesses and generated C, with `-fsanitize=undefined,address` |
| GNU make, bash | Ubuntu 24.04 | the `Makefile` |
| Python | 3.12 + venv | the recompiler; `make venv` installs `requirements.txt` (capstone `>=5,<6`, pyelftools) |
| libunicorn-dev | 2.0.1 (Ubuntu 24.04 package) | reference CPU for the M3–M5/real tests (`-lunicorn`) |
| devkitPro: devkitARM, libctru | current `3ds-dev` group | `arm-none-eabi-gcc` builds the test programs |
| devkitPro: picasso | in `$DEVKITPRO/tools/bin` | assembles the GPU test shaders |
| devkitPro: 3ds-examples | `/opt/devkitpro/examples/3ds` | `test-compat-quick` builds 4 of them |

The Makefile expects `DEVKITPRO=/opt/devkitpro` and `DEVKITARM` set (it puts
`$DEVKITARM/bin` on `PATH`). `make test` runs these targets in order:
`test-m0 … test-m5 test-real test-gpu test-m6 test-m11 test-m13 test-dsp
test-y2r test-compat-quick`.

## Suggested approach

1. Use the official devkitPro container (`devkitpro/devkitarm` on Docker
   Hub) as the job's `container:`. **Check** that it has libctru, picasso
   and the 3ds examples; if not, install `3ds-dev` with `dkp-pacman` inside
   it. Gotcha: `apt.devkitpro.org` sits behind Cloudflare and returns 403 to
   plain `wget`, so prefer the container over the install script.
2. Install the host tools in the job: `clang`, `make`, `python3-venv`,
   `libunicorn-dev` (Unicorn **2.0.1**; the tests diff against its exact
   behaviour, so if the container's distro ships another version, say which
   and whether the tests still pass).
3. `make venv && make test`.
4. ASan in containers: LeakSanitizer can fail without ptrace. If it does,
   prefer `options: --cap-add SYS_PTRACE` on the container over turning
   leak checks off; if you must use `ASAN_OPTIONS=detect_leaks=0`, say so
   in the workflow comment.
5. Cache what's safe to cache: the pip download cache, and the devkitPro
   container image. **Don't** cache `out/` between runs: the tests must
   build from scratch.

## Rules

- No game data, ROMs, keys or firmware anywhere: none is needed, and the
  workflow must not download any (see `CLAUDE.md`, "Hard rules").
- Change only `.github/workflows/test.yml` and, for a status badge, one line
  near the top of `README.md`. If a test turns out to depend on something
  machine-specific, don't edit the test: report it (file, line, why).
- Keep the workflow readable: comments for anything non-obvious, in the
  repository's plain style.

## Done when

- The workflow passes on a fork (link the run).
- You report: total runtime, the versions it actually used (clang, Unicorn,
  devkitARM, libctru), and anything you had to work around.
- You hand back the workflow file (and README line) as a diff or a branch.
