#!/usr/bin/env bash
set -Eeuo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CC_BIN="${CC:-cc}"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT
"$CC_BIN" -std=c11 -Wall -Wextra -Werror -pedantic -O2 -I"$ROOT/kernel/include" \
  "$ROOT/tests/ps2_keyboard_test.c" "$ROOT/kernel/ps2_keyboard.c" \
  "$ROOT/kernel/ps2_mouse.c" "$ROOT/kernel/hw_stack.c" \
  -o "$TMP_DIR/ps2_keyboard_test"
"$TMP_DIR/ps2_keyboard_test"
