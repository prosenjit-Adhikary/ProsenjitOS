#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ASM="$ROOT/kernel/idt.asm"

# Exceptions that architecturally push an error code on modern x86-64.
# Vectors 21 (#CP), 29 (#VC, AMD SEV-ES), and 30 (#SX) matter on CPUs
# implementing those extensions; their stubs must not add a synthetic zero.
for vector in 8 10 11 12 13 14 17 21 29 30; do
  if ! grep -Eq "^ISR_WITH_ERROR ${vector}$" "$ASM"; then
    echo "FAIL: exception vector ${vector} must use ISR_WITH_ERROR" >&2
    exit 1
  fi
done

# Catch regressions that silently reintroduce a synthetic error slot.
for vector in 8 10 11 12 13 14 17 21 29 30; do
  if grep -Eq "^ISR_NO_ERROR ${vector}$" "$ASM"; then
    echo "FAIL: exception vector ${vector} incorrectly uses ISR_NO_ERROR" >&2
    exit 1
  fi
done

echo "PASS: all modern x86-64 error-code exception stubs use consistent frames"
