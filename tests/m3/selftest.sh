#!/bin/sh
# Usage: selftest.sh PYTHON CC CFLAGS...
#
# Plant a translator bug (ldrsh without sign extension), rebuild the M2
# addressing-mode program with tracing, and check that the differential
# driver fails AND points at the ldrsh instruction and the wrong register.
set -u
py=$1 cc=$2
shift 2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

"$py" tests/m3/plant_bug.py out/m2/addr.elf "$tmp/gen_addr.c" > /dev/null || exit 1
"$cc" "$@" -DRECOMP_TRACING=1 tests/m3/diff.c "$tmp/gen_addr.c" runtime/runtime.c -lunicorn -o "$tmp/diff" || exit 1

if timeout 120 "$tmp/diff" 50 > "$tmp/out" 2>&1; then
    echo "FAIL selftest: the planted ldrsh bug was not detected"
    exit 1
fi
if ! grep -q 'after [0-9a-f]\{8\} `ldrsh r1' "$tmp/out" || ! grep -q '^    r1 *recompiled' "$tmp/out"; then
    echo "FAIL selftest: bug detected but not pinpointed at the ldrsh / r1:"
    sed -n '/DIVERGENCE/,$p' "$tmp/out"
    exit 1
fi
echo "PASS selftest: planted bug caught and pinpointed:"
sed -n '/DIVERGENCE/,$p' "$tmp/out" | sed 's/^/    /'
