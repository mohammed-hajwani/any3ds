# Contributing to any3ds

Thanks for looking. This is a side project, so reviews may take a few days.

## The legal rules (non-negotiable)

- **Never commit** game code, ROMs, dumps, decryption keys, firmware, or
  recompiled output of commercial games. Keep all of it in `dumps/` and
  `out/`, which are gitignored.
- **Never post, request or link** ROMs or keys: not in code, issues, PRs or
  discussions. Such issues and PRs are closed without discussion.
- Game analysis (decompiled code, memory dumps, traces, replay inputs) stays
  on your machine.
- Metadata is fine: compatibility reports, per-game configs (addresses,
  patch points) and dump hashes.
- Code from other emulators can only come in if its licence is compatible
  with GPL v2 or later, and it must say where it came from.

## Building and testing

Everything runs in Linux (WSL2 Ubuntu 24.04 is what the owner uses); see
"Setup" in the [README](README.md). Before sending a PR:

```bash
make test
```

It must pass. CI runs the same on every pull request.

## Changes

1. **Open an issue first** for anything bigger than a small fix, so we can
   agree on the approach before you spend time on it.
2. Branch off `main`, keep changes small, one topic per PR.
3. Write the test first. A new instruction needs a test that runs it
   against Unicorn (`tests/real/*.S`), including its flags if it sets any.
   A GPU change needs a check in `make test-gpu` or a compat example.
4. An unsupported instruction or OS call is a **hard error** that names it.
   Never skip anything silently.
5. Keep the generated C readable: each instruction keeps its address and
   disassembly as a comment.
6. Ask before adding dependencies or restructuring the layout.
7. Explain ARM and 3DS gotchas in comments where you handle them.

## Reporting a game

Use the "Game or bug report" form. Include the game's title ID, your dump's
SHA-256, and the log from the point it stops. The recompiler's errors name
the address and instruction, and the OS layer names the call it stopped at,
so those lines are usually all we need.
