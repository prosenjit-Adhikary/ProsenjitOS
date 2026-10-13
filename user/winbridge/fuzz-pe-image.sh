#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
: "${FUZZ_SECONDS:=60}"
case "$FUZZ_SECONDS" in
    ''|*[!0-9]*) echo "FUZZ_SECONDS must be a positive integer" >&2; exit 2 ;;
esac
if [ "$FUZZ_SECONDS" -lt 1 ]; then
    echo "FUZZ_SECONDS must be a positive integer" >&2
    exit 2
fi
OUT="${TMPDIR:-/tmp}/winbridge-pe-fuzz-$$"
trap 'rm -f "$OUT"' EXIT HUP INT TERM
"${CLANG:-clang}" -std=c11 -Wall -Wextra -Werror -O1 -g \
    -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -I"$ROOT" \
    "$ROOT/pe_image.c" "$ROOT/fuzz_pe_image.c" -o "$OUT"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
    "$OUT" -max_total_time="$FUZZ_SECONDS" -print_final_stats=1
