#!/usr/bin/env bash
set -Eeuo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CC_BIN="${CC:-cc}"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT
"$CC_BIN" -std=c11 -Wall -Wextra -Werror -pedantic -O2 \
  -I"$ROOT/kernel/include" \
  "$ROOT/tests/xhci_capability_test.c" "$ROOT/kernel/xhci.c" \
  -o "$TMP_DIR/xhci_capability_test"
"$TMP_DIR/xhci_capability_test"
echo "xHCI capability register validation tests passed"
