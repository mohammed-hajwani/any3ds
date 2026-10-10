#!/bin/sh
# Usage: selftest.sh PYTHON CC BUG ELF INSN_REGEX REG CFLAGS...
#
# Plant a translator bug (see plant_bug.py), rebuild ELF with tracing, and
# check that the differential driver fails AND pinpoints it: the report
# must name an instruction matching INSN_REGEX and show REG differing.
set -u
py=$1 cc=$2 bug=$3 elf=$4 insn=$5 reg=$6
shift 6
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

"$py" tests/m3/plant_bug.py "$bug" "$elf" "$tmp/gen.c" > /dev/null || exit 1
"$cc" "$@" -DRECOMP_TRACING=1 tests/m3/diff.c "$tmp/gen.c" runtime/runtime.c -lunicorn -o "$tmp/diff" || exit 1

if timeout 120 "$tmp/diff" 50 > "$tmp/out" 2>&1; then
    echo "FAIL selftest ($bug): the planted bug was not detected"
    exit 1
fi
if ! grep -q "after [0-9a-f]\{8\} \`$insn" "$tmp/out" || ! grep -q "^    $reg *recompiled" "$tmp/out"; then
    echo "FAIL selftest ($bug): detected but not pinpointed at \`$insn\` / $reg:"
    sed -n '/DIVERGENCE/,$p' "$tmp/out"
    exit 1
fi
echo "PASS selftest ($bug): planted bug caught and pinpointed:"
sed -n '/DIVERGENCE/,$p' "$tmp/out" | sed 's/^/    /'
