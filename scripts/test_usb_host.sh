#!/usr/bin/env bash
set -Eeuo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CC_BIN="${CC:-cc}"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT
"$CC_BIN" -std=c11 -Wall -Wextra -Werror -pedantic -O2 \
  -I"$ROOT/kernel/include" \
  "$ROOT/tests/usb_host_test.c" "$ROOT/kernel/usb_host.c" \
  -o "$TMP_DIR/usb_host_test"
"$TMP_DIR/usb_host_test"
echo "PCI USB host-controller classification tests passed"
