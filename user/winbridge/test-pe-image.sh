#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OUT="${TMPDIR:-/tmp}/winbridge-pe-test-$$"
trap 'rm -f "$OUT"' EXIT HUP INT TERM
${CC:-cc} -std=c11 -Wall -Wextra -Werror -O2 -I"$ROOT" "$ROOT/pe_image.c" "$ROOT/test_pe_image.c" -o "$OUT"
"$OUT"
