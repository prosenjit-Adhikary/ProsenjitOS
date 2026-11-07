#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC="${CC:-cc}"
OUT="${TMPDIR:-/tmp}/prosenjitos-pe-inspect-test-$$"
trap 'rm -f "$OUT"' EXIT HUP INT TERM

"$CC" -std=c11 -Wall -Wextra -Werror -O2 \
    -I"$ROOT/kernel/include" \
    "$ROOT/kernel/pe_inspect.c" \
    "$ROOT/tests/pe_inspect_host_test.c" \
    -o "$OUT"
"$OUT"
printf '%s\n' "PE inspection host tests: PASS"
