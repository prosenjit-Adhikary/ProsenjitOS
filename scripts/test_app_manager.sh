#!/usr/bin/env bash
set -Eeuo pipefail

# Host-only regression suite; no kernel image or runner-specific APIs required.

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CC_BIN="${CC:-cc}"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

"$CC_BIN" \
  -std=c11 -Wall -Wextra -Werror -pedantic -O2 \
  -I"$ROOT/user/apps" \
  "$ROOT/user/apps/app_manager.c" \
  "$ROOT/tests/app_manager_test.c" \
  -o "$TMP_DIR/app_manager_test"

"$CC_BIN" \
  -std=c11 -Wall -Wextra -Werror -pedantic -O2 \
  -I"$ROOT/user/apps" \
  "$ROOT/user/apps/app_format.c" \
  "$ROOT/tests/app_format_test.c" \
  -o "$TMP_DIR/app_format_test"

"$TMP_DIR/app_manager_test"
"$TMP_DIR/app_format_test"
