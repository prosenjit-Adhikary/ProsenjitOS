#!/usr/bin/env bash
set -Eeuo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CC_BIN="${CC:-cc}"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT
"$CC_BIN" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -pedantic -O2 -pthread \
  -I"$ROOT/kernel/include" \
  "$ROOT/tests/memory_allocator_test.c" "$ROOT/kernel/memory.c" \
  -o "$TMP_DIR/memory_allocator_test"
"$TMP_DIR/memory_allocator_test"
