#!/bin/bash
# Build a native Windows program from a game recompiled by tools/build_dump.sh:
#
#   tools/build_win.sh out/game          ->  out/game/win/run.exe
#   OS_ONLY=1 tools/build_win.sh out/game     relink after hle/, gpu/ or runtime/ changes
#
# Then, on Windows: out\game\win\run.exe --romfs DUMP --window 2 ...
# (runner options in tests/m6/runner.c; --window draws with Vulkan).
#
# Cross-compiled in WSL with clang --target=x86_64-w64-mingw32 and linked
# statically, so run.exe needs nothing but the GPU driver's vulkan-1.dll.
# Needs: apt install mingw-w64 libvulkan-dev (the Vulkan headers).
set -e
cd "$(dirname "$0")/.."
out=${1:?usage: tools/build_win.sh OUT_DIR (made by tools/build_dump.sh)}
[ -f "$out/gen.c" ] || { echo "no $out/gen.c: run tools/build_dump.sh first"; exit 1; }
win="$out/win"
mkdir -p "$win/cro"
jobs=$(nproc)
gen_jobs=$(( jobs < 8 ? jobs : 8 ))
start=$(date +%s)
since() { echo "   ($(( $(date +%s) - start )) s so far)"; }

# Only the Vulkan headers: all of /usr/include would bring Linux's C library in.
vkinc=/tmp/any3ds-vkinc
mkdir -p "$vkinc"
ln -sfn /usr/include/vulkan "$vkinc/vulkan"
ln -sfn /usr/include/vk_video "$vkinc/vk_video"

CC="clang --target=x86_64-w64-mingw32"
GEN_FLAGS="-std=c11 -O1 -g0 -w -fno-math-errno -Iruntime"
# MinGW's GCC runtime uses emulated TLS. Clang's native _Thread_local
# accesses crash even in a standalone pthread probe with this static link.
OS_FLAGS="-std=c11 -O1 -Wall -Wextra -fno-math-errno -femulated-tls -D_FILE_OFFSET_BITS=64 -isystem $vkinc -Iruntime -Ihle -Igpu -pthread"

if [ "${OS_ONLY:-0}" != 1 ] || [ ! -f "$win/gen.o" ]; then
    echo "== compiling the game for Windows ($gen_jobs jobs)"
    for f in "$out"/gen*.c "$out"/cro/*.c "$out/modules.c"; do echo "$f"; done \
        | xargs -P "$gen_jobs" -I{} sh -c "$CC $GEN_FLAGS -c {} -o \"$win/\$(echo {} | sed 's#^$out/##; s/\\.c\$/.o/')\""
    since
fi
for f in tests/m6/runner.c runtime/runtime.c hle/*.c gpu/*.c; do echo "$f"; done \
    | xargs -P "$jobs" -I{} sh -c "$CC $OS_FLAGS -c {} -o $win/os_\$(basename {} .c).o"

echo "== linking $win/run.exe"
# The guest main thread runs on the process main thread: give it the 16 MB
# that guest threads get (HOST_STACK in hle/kernel.c), not Windows' 1 MB.
$CC -static -pthread -Wl,--stack,16777216 -Wl,-Map,"$win/run.map" "$win"/gen*.o "$win"/cro/*.o "$win/modules.o" "$win"/os_*.o -lm -o "$win/run.exe"
echo "built $win/run.exe"
since
