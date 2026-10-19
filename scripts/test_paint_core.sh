#!/usr/bin/env bash
set -Eeuo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CC_BIN="${CC:-cc}"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT
"$CC_BIN" -std=c11 -Wall -Wextra -Werror -pedantic -O2 \
  "$ROOT/user/apps/paint/paint_core.c" \
  "$ROOT/tests/paint_core_test.c" -o "$TMP_DIR/paint_core_test"
(cd "$TMP_DIR" && ./paint_core_test)
