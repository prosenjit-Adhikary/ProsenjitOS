#!/usr/bin/env bash
set -Eeuo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CC_BIN="${CC:-cc}"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT
"$CC_BIN" -std=c11 -Wall -Wextra -Werror -pedantic -O2 \
  -DPROS_PIC_HOSTED_TEST -I"$ROOT/kernel/include" \
  "$ROOT/tests/pic_test.c" "$ROOT/kernel/pic.c" \
  -o "$TMP_DIR/pic_test"
"$TMP_DIR/pic_test"
