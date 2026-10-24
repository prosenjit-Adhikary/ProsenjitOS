#!/usr/bin/env bash
set -Eeuo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CC_BIN="${CC:-cc}"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT
"$CC_BIN" -std=c11 -Wall -Wextra -Werror -pedantic -O2 -I"$ROOT/kernel/include" "$ROOT/tests/hw_stack_test.c" "$ROOT/kernel/hw_stack.c" -o "$TMP_DIR/hw_stack_test"
"$TMP_DIR/hw_stack_test"
