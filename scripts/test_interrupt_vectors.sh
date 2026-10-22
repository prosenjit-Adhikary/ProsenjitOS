#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ASM="$ROOT/kernel/idt.asm"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

# Extract a named assembly entry block, excluding the next entry label.
block_has() {
  local start="$1" end="$2" pattern="$3" description="$4"
  if ! awk -v start="$start" -v end="$end" '
    $0 ~ ("^" start ":$") { active=1 }
    active { print }
    active && $0 ~ ("^" end ":$") { exit }
  ' "$ASM" | grep -Eq "$pattern"; then
    fail "$description"
  fi
}

# Exceptions that architecturally push an error code on modern x86-64.
# Vectors 21 (#CP), 29 (#VC, AMD SEV-ES), and 30 (#SX) must not add a
# synthetic error slot.
for vector in 8 10 11 12 13 14 17 21 29 30; do
  grep -Eq "^ISR_WITH_ERROR ${vector}$" "$ASM" ||
    fail "exception vector ${vector} must use ISR_WITH_ERROR"
  if grep -Eq "^ISR_NO_ERROR ${vector}$" "$ASM"; then
    fail "exception vector ${vector} incorrectly uses ISR_NO_ERROR"
  fi
done

# Check that each C-calling entry path aligns the temporary call stack and
# restores the saved interrupt-frame pointer rather than the aligned RSP.
block_has isr128 isr32 'and rsp, -16' "syscall entry must align the C-call stack"
block_has isr128 isr32 'mov rsp, r12' "syscall return must restore the saved frame"
block_has isr32 '%macro ISR_NO_ERROR' 'and rsp, -16' "timer entry must align the C-call stack"
block_has isr32 '%macro ISR_NO_ERROR' 'mov rsp, r12' "timer path must restore a saved frame pointer"
block_has isr_common 'section .note.GNU-stack' 'and rsp, -16' "exception entry must align the C-call stack"
block_has isr_common 'section .note.GNU-stack' 'mov rsp, r12' "exception path must restore the saved frame pointer"

# Require every GPR to be saved and restored by each returning entry path.
# This is a source-structure regression guard, not a substitute for runtime
# register-sentinel tests on actual interrupt delivery.
for entry in isr128 isr32 isr_common; do
  case "$entry" in
    isr128) end=isr32 ;;
    isr32) end='%macro ISR_NO_ERROR' ;;
    isr_common) end='section .note.GNU-stack' ;;
  esac
  for reg in rax rbx rcx rdx rsi rdi rbp r8 r9 r10 r11 r12 r13 r14 r15; do
    block_has "$entry" "$end" "^[[:space:]]*push ${reg}$" "$entry must save ${reg}"
    block_has "$entry" "$end" "^[[:space:]]*pop ${reg}$" "$entry must restore ${reg}"
  done
done

# The timer must acknowledge the PIC on both switch and no-switch paths.
block_has isr32 '%macro ISR_NO_ERROR' 'call pic_eoi_master' "timer path must acknowledge the master PIC"

echo "PASS: exception error-code frames, GPR save/restore coverage, and interrupt stack-alignment structure"
