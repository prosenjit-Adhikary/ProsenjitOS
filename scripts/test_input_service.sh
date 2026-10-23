#!/usr/bin/env bash
set -Eeuo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CC_BIN="${CC:-cc}"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT
"$CC_BIN" -std=c11 -Wall -Wextra -Werror -pedantic -O2 \
  -I"$ROOT/kernel/include" \
  "$ROOT/tests/input_service_test.c" "$ROOT/kernel/input_service.c" \
  "$ROOT/kernel/ps2_keyboard.c" "$ROOT/kernel/hw_stack.c" "$ROOT/kernel/object.c" \
  -o "$TMP_DIR/input_service_test"
"$TMP_DIR/input_service_test"
