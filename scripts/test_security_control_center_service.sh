#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CC_BIN="${CC:-cc}"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

SOURCES=(
  "$ROOT/user/security/telemetry_protocol.c"
  "$ROOT/user/security/telemetry_service.c"
  "$ROOT/tests/security_control_center_service_test.c"
)
INCLUDES=(-I"$ROOT/user/security")

# Run the functional test without sanitizers first so assertion/test failures
# remain distinguishable from sanitizer-runtime failures in CI logs.
"$CC_BIN" -std=c11 -Wall -Wextra -Werror -pedantic -O2 \
  "${INCLUDES[@]}" "${SOURCES[@]}" \
  -o "$TMP_DIR/security_control_center_service_test"
"$TMP_DIR/security_control_center_service_test"

# Run the same cases with memory/UB instrumentation and useful diagnostics.
"$CC_BIN" -std=c11 -Wall -Wextra -Werror -pedantic -O1 -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  "${INCLUDES[@]}" "${SOURCES[@]}" \
  -o "$TMP_DIR/security_control_center_service_test_san"
ASAN_OPTIONS="${ASAN_OPTIONS:-abort_on_error=1}" \
UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}" \
  "$TMP_DIR/security_control_center_service_test_san"

echo "security control center service dispatcher tests passed"
