#!/bin/sh
# Recompile and build your own decrypted dump (a .3ds, .cxi, or a folder with
# code.bin + exheader.bin) into a host program that runs it on our HLE.
#
#   tools/build_dump.sh DUMP [OUT_DIR]        (default OUT_DIR: out/dump)
#   OUT_DIR/run --dump frame.bmp ...           (same options as tests/m6/runner.c)
#   OS_ONLY=1 tools/build_dump.sh DUMP          rebuild only the OS layer (after hle/ changes)
#
# Everything generated stays in OUT_DIR, under out/ (gitignored): recompiled
# game code must never be committed. Functions the translator can't handle
# yet become stubs that stop loudly if called (--keep-going); they are listed
# in OUT_DIR/gen.failures.txt.
set -eu
dump=${1:?usage: tools/build_dump.sh DUMP [OUT_DIR]}
out=${2:-out/dump}
case "$out" in out/*) ;; *) echo "OUT_DIR must be under out/ (gitignored)"; exit 1 ;; esac
jobs=$(nproc)
mkdir -p "$out"

# OS_ONLY=1: keep the recompiled game, rebuild just the OS layer (hle/, gpu/).
if [ "${OS_ONLY:-0}" = 1 ] && [ -f "$out/gen.o" ]; then
    echo "== keeping the recompiled game in $out (OS_ONLY=1)"
else
    echo "== recompiling $dump"
    rm -f "$out"/gen*.c "$out"/gen*.o
    .venv/bin/python -m recomp "$dump" -o "$out/gen.c" --keep-going --split 400 > "$out/recomp.log"
    grep -v '^  f_' "$out/recomp.log"
fi

# Generated code: unoptimised and without sanitizers, or a big game takes
# hours to compile. The OS layer is optimised a little: it runs every frame.
GEN_FLAGS="-std=c11 -O0 -g0 -w -fno-math-errno -Iruntime"
OS_FLAGS="-std=c11 -O1 -g -Wall -Wextra -fno-math-errno -Iruntime -Ihle -Igpu -pthread"

if [ "${OS_ONLY:-0}" != 1 ] || [ ! -f "$out/gen.o" ]; then
    echo "== compiling $(ls "$out"/gen*.c | wc -l) C files with $jobs jobs"
    ls "$out"/gen*.c | xargs -P "$jobs" -I{} sh -c 'clang '"$GEN_FLAGS"' -c {} -o "$(echo {} | sed "s/\.c$/.o/")"'
fi
for f in tests/m6/runner.c runtime/runtime.c hle/*.c gpu/*.c; do
    echo "$f"
done | xargs -P "$jobs" -I{} sh -c 'clang '"$OS_FLAGS"' -c {} -o '"$out"'/os_$(basename {} .c).o'

echo "== linking $out/run"
clang -pthread "$out"/gen*.o "$out"/os_*.o -lm -o "$out/run"
echo "built $out/run"
