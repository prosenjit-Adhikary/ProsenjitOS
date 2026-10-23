#!/usr/bin/env bash
set -Eeuo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CC_BIN="${CC:-cc}"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT
"$CC_BIN" -std=c11 -Wall -Wextra -Werror -pedantic -O2 \
  -I"$ROOT/kernel/include" \
  "$ROOT/tests/usb_descriptor_test.c" "$ROOT/kernel/usb_descriptor.c" \
  -o "$TMP_DIR/usb_descriptor_test"
"$TMP_DIR/usb_descriptor_test"
echo "USB configuration/HID endpoint descriptor tests passed"
