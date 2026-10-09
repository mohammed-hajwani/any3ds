#!/bin/sh
# Usage: expect_reject.sh PYTHON INPUT REGEX
#
# The recompiler must refuse INPUT, print an error matching REGEX, and
# write no output.
set -u
py=$1 input=$2 pattern=$3
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

if "$py" -m recomp "$input" -o "$tmp/gen.c" 2> "$tmp/err" > /dev/null; then
    echo "FAIL: $input was accepted"
    exit 1
fi
if ! grep -Eq "$pattern" "$tmp/err"; then
    echo "FAIL: $input: unexpected error:"
    cat "$tmp/err"
    exit 1
fi
if [ -e "$tmp/gen.c" ]; then
    echo "FAIL: $input: output written despite error"
    exit 1
fi
echo "PASS rejects $(basename "$input"): $(cat "$tmp/err")"
