# Brief: sound output (DSP mixing, then the speakers)

Read `CLAUDE.md` first, then the comment at the top of `hle/dsp.c`. You
can do and test all of this without any game: libctru's ndsp examples
(`make test-dsp`, and `/opt/devkitpro/examples/3ds/audio/`) drive the DSP.
Don't push; report back with a diff.

## Goal

The DSP is silent today: `hle/dsp.c` consumes each input voice at the right
rate (so code waiting for a buffer carries on) but makes no samples. Make it
produce the mixed audio the 3DS would, and play it on Windows. AAC (codec
1, which Pokémon X uses for music) stays silent: decoding it is a separate
job. Sound effects in PCM and ADPCM should become audible.

## Rules that matter here

- **Determinism.** Runs are bit-for-bit reproducible (replays and tests rely
  on it). Mixing must happen inside `dsp_frame`, on the virtual clock (160
  samples per DSP frame), never driven by the sound card. The host output
  only consumes what was mixed: if it falls behind or runs ahead, drop or
  repeat whole frames at the output, not in the emulation.
- **Our own code from public docs** (3dbrew's DSP pages, libctru's ndsp
  headers and source). No code from other emulators. Where docs are silent,
  say so in a comment, and log the guess once when it's used.
- **No new dependencies** without asking. Windows output can use winmm
  (`waveOut*`) or WASAPI, both part of Windows and available in the MinGW
  cross build (`tools/build_win.sh`); say which and why.

## Steps

1. **Decode** each input's buffers: PCM8, PCM16 (mono/stereo) and DSP
   ADPCM (4-bit, with the per-input coefficients in `OFF_ADPCM_COEFS` and
   the predictor/history from the buffer's config). Buffers chain and loop as
   the input config says; `play()` already walks positions, so keep its
   bookkeeping and add the samples.
2. **Resample** each input to the output rate with its rate multiplier
   (3dbrew documents the interpolation modes: none, linear, polyphase; do
   none and linear, and name polyphase's stop if a program uses it).
3. **Mix**: per-input gains into the three mixes (main, aux A, aux B), the
   master volume, then the frame's stereo output into `OFF_OUTPUT` (the app
   can read it back) as signed 16-bit. Aux effects and the compressor can be
   pass-through for now, if documented as such.
4. **Out**:
   - On every platform: `--wav FILE` writes the stream to a WAV file. Tests
     use it.
   - On Windows: play it through the device when `--window` is used.
     `--mute` turns that off.
5. **Tests**: a small libctru program in `tests/dsp/` (next to the existing
   one) that plays a known PCM16 tone and an ADPCM buffer. Check the WAV
   output against a golden hash, and check the sample count matches the
   virtual time exactly.

## Done when

- `make test` still passes, and the new DSP test passes.
- On Windows, a devkitPro audio example is audible: cross-build it for
  Windows the way `tools/build_win.sh` builds a game, and say how in the
  report.
- Report: what's implemented, what's guessed (with the reason), what stops
  by name, and how you checked the ADPCM decoder.
