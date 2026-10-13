#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OUT="${TMPDIR:-/tmp}/winbridge-pe-sanitize-$$"
trap 'rm -f "$OUT"' EXIT HUP INT TERM
${CC:-cc} -std=c11 -Wall -Wextra -Werror -O1 -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer -I"$ROOT" \
    "$ROOT/pe_image.c" "$ROOT/test_pe_image.c" -o "$OUT"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$OUT"
