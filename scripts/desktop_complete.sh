#!/usr/bin/env bash
set -Eeuo pipefail
ROOT="$(git rev-parse --show-toplevel 2>/dev/null || true)"
if [[ -z "$ROOT" ]]; then echo "ERROR: run from the ProsenjitOS repository." >&2; exit 2; fi
cd "$ROOT"
REPORT="reports/desktop-integration-report.md"
mkdir -p reports
failures=()
notes=()
failure_count=0
note() { notes+=("$1"); }
fail() { failures+=("$1"); failure_count=$((failure_count + 1)); }
need_cmd() { command -v "$1" >/dev/null 2>&1 || fail "missing command: $1"; }

echo "[1/6] Architecture boundary"
if grep -RInE '(^|[^[:alnum:]_])(XFCE|Xorg|Wayland|X11)([^[:alnum:]_]|$)' kernel --include='*.c' --include='*.h' --include='*.asm' --include='*.S' >/tmp/prosenjitos_kernel_gui_hits 2>/dev/null; then
  fail "desktop-stack reference detected inside kernel/; inspect /tmp/prosenjitos_kernel_gui_hits"
else
  note "kernel GUI-stack contamination check: clean"
fi

echo "[2/6] Build prerequisites"
for c in git make gcc ld nasm qemu-system-x86_64; do need_cmd "$c"; done

echo "[3/6] User-mode and graphics foundations"
grep -RIlE 'ELF|elf' kernel include user --include='*.c' --include='*.h' --include='*.asm' >/dev/null 2>&1 || fail "ELF/user-program loader foundation not detected"
grep -RIlE 'init|service manager|service_manager' kernel user include --include='*.c' --include='*.h' --include='*.asm' >/dev/null 2>&1 || fail "init/service-manager foundation not detected"
grep -RIlE 'CPL3|user_mode|USER_TEST|process_create' kernel --include='*.c' --include='*.h' --include='*.asm' >/dev/null 2>&1 || fail "userspace process-entry foundation not detected"
grep -RIlE 'display|framebuffer|graphics|input' kernel include drivers --include='*.c' --include='*.h' --include='*.asm' >/dev/null 2>&1 || fail "display/input boundary not detected"

echo "[4/6] Local build"
if ! make clean >/tmp/prosenjitos_make_clean.log 2>&1; then fail "make clean failed; see /tmp/prosenjitos_make_clean.log"; fi
if ! make iso >/tmp/prosenjitos_make_iso.log 2>&1; then fail "make iso failed; see /tmp/prosenjitos_make_iso.log"; fi

echo "[5/6] QEMU smoke test"
if [[ -f build/prosenjitOS.iso ]]; then
  rm -f build/desktop-qemu-serial.log build/desktop-qemu-debug.log
  timeout 20s qemu-system-x86_64 -display none -serial file:build/desktop-qemu-serial.log -no-reboot -no-shutdown -cdrom build/prosenjitOS.iso -d int -D build/desktop-qemu-debug.log >/tmp/prosenjitos_qemu_stdout.log 2>/tmp/prosenjitos_qemu_stderr.log || true
  [[ -s build/desktop-qemu-serial.log ]] && note "QEMU serial smoke log generated" || fail "QEMU produced no serial log"
else
  fail "build/prosenjitOS.iso was not produced"
fi

echo "[6/6] Report"
{
  echo "# ProsenjitOS Desktop Integration Gate"
  echo
  echo "Generated: $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
  echo
  echo "## Rules"
  echo "- Desktop environment is userspace."
  echo "- Existing Xorg/X11 or Wayland and XFCE are integration targets."
  echo "- No desktop stack is added to kernel-core."
  echo "- GitHub-hosted Actions are not required."
  echo
  echo "## Notes"
  for n in $notes; do echo "- $n"; done
  echo
  echo "## Blockers"
  if ((failure_count > 0)); then
    for f in $failures; do echo "- $f"; done
  else
    echo "none"
  fi
} > "$REPORT"

if ((failure_count > 0)); then
  echo "DESKTOP GATE: NOT COMPLETE"
  for f in $failures; do echo " - $f"; done
  echo "Report: $REPORT"
  exit 1
fi
echo "DESKTOP GATE: PASS"
echo "Report: $REPORT"
