#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CC_BIN="${CC:-cc}"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

"$CC_BIN" \
  -std=c11 -Wall -Wextra -Werror -pedantic -O2 \
  -I"$ROOT/user/desktop" \
  "$ROOT/user/desktop/desktop_manager.c" \
  "$ROOT/tests/desktop_manager_test.c" \
  -o "$TMP_DIR/desktop_manager_test"

"$TMP_DIR/desktop_manager_test"
