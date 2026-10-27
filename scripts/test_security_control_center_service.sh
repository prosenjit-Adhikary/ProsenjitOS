#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT
cc -std=c11 -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined \
  -I"$root/user/security" \
  "$root/user/security/telemetry_protocol.c" \
  "$root/user/security/telemetry_service.c" \
  "$root/tests/security_control_center_service_test.c" -o "$tmp"
"$tmp"
