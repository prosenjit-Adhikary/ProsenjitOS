#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CC_BIN="${CC:-cc}"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

"$CC_BIN" \
  -std=c11 -Wall -Wextra -Werror -pedantic -O2 \
  -I"$ROOT/user/security" \
  "$ROOT/user/security/telemetry_protocol.c" \
  "$ROOT/tests/security_control_center_protocol_test.c" \
  -o "$TMP_DIR/security_control_center_protocol_test"

"$TMP_DIR/security_control_center_protocol_test"

if [[ "${SCC_SANITIZERS:-0}" == "1" ]]; then
  "$CC_BIN" \
    -std=c11 -Wall -Wextra -Werror -pedantic -O1 -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$ROOT/user/security" \
    "$ROOT/user/security/telemetry_protocol.c" \
    "$ROOT/tests/security_control_center_protocol_test.c" \
    -o "$TMP_DIR/security_control_center_protocol_test_san"
  "$TMP_DIR/security_control_center_protocol_test_san"
fi
