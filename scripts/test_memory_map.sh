#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CC_BIN="${CC:-cc}"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

"$CC_BIN" \
  -std=c11 -Wall -Wextra -Werror -pedantic -O2 -pthread \
  -I"$ROOT/kernel/include" \
  "$ROOT/tests/memory_map_test.c" \
  -o "$TMP_DIR/memory_map_test"

"$TMP_DIR/memory_map_test"
