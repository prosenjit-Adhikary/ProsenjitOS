#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ASM="$ROOT/kernel/idt.asm"
PIC="$ROOT/kernel/pic.c"

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
    active && $0 ~ ("^" end) { exit }
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

# Do not inherit BIOS/bootloader PIC masks: only IRQ0 may be enabled at
# initialization, and all slave PIC IRQs must remain masked until registered.
grep -Fq 'pic_out8(0x21, 0xFEu);' "$PIC" ||
  fail "PIC initialization must enable only IRQ0"
grep -Fq 'pic_out8(0xA1, 0xFFu);' "$PIC" ||
  fail "PIC initialization must keep every slave IRQ masked"
if grep -Fq 'master_mask' "$PIC"; then
  fail "PIC initialization must not inherit firmware master IRQ mask"
fi

echo "PASS: exception frames, GPR save/restore, stack alignment, and safe PIC masks"
