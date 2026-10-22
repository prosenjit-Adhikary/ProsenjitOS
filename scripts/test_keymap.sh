#!/usr/bin/env bash
set -Eeuo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CC_BIN="${CC:-cc}"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT
"$CC_BIN" -std=c11 -Wall -Wextra -Werror -pedantic -O2 \
  "$ROOT/tests/keymap_test.c" "$ROOT/user/desktop/keymap.c" \
  -o "$TMP_DIR/keymap_test"
"$TMP_DIR/keymap_test"
echo "US keyboard keymap tests passed"
