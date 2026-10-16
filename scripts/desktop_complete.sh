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
for c in git make gcc ld nasm qemu-system-x86_64 timeout; do need_cmd "$c"; done

echo "[3/6] User-mode and graphics foundations"
# A filename/comment containing "ELF" is not evidence of an ELF loader. Require
# an implementation-shaped symbol/header reference instead.
if ! grep -RIlE 'pros_elf_load_with_args|pros_elf_load[[:space:]]*\(' kernel --include='*.c' --include='*.h' >/dev/null 2>&1; then
  fail "kernel ELF loading entry point was not detected"
else
  note "kernel ELF loading entry point detected (not proof of userspace session startup)"
fi
# Deliberately search userspace only: a kernel-resident service manager is not
# an acceptable substitute for the planned userspace service lifecycle.
if ! grep -RIlE 'service_manager_init|service_spawn|init_main|userspace_service_manager' user --include='*.c' --include='*.h' --include='*.asm' >/dev/null 2>&1; then
  fail "userspace init/service-manager implementation was not detected (kernel service manager does not satisfy this gate)"
else
  note "userspace init/service-manager implementation-shaped symbols detected; runtime behavior still requires verification"
fi
# Headers and kernel drivers are contracts/primitives, not proof of a usable
# userspace input event path or display service.
if ! grep -RIlE 'display_service|input_service|display_server_main|input_server_main' user --include='*.c' --include='*.h' --include='*.asm' >/dev/null 2>&1; then
  fail "userspace display/input service implementation was not detected"
else
  note "userspace display/input service implementation-shaped symbols detected; end-to-end delivery still requires runtime verification"
fi
if ! grep -RIlE 'CPL3|USER_MODE|user_mode|iretq|sysret|process_create' kernel --include='*.c' --include='*.h' --include='*.asm' >/dev/null 2>&1; then
  fail "userspace process-entry foundation not detected"
else
  note "userspace process-entry foundation detected"
fi
if ! grep -RIlE 'display|framebuffer|graphics|input' kernel include drivers --include='*.c' --include='*.h' --include='*.asm' >/dev/null 2>&1; then
  fail "display/input boundary not detected"
else
  note "display/input boundary detected"
fi

echo "[4/6] DML unit test and local build"
if ! make desktop-unit-test >/tmp/prosenjitos_desktop_unit_test.log 2>&1; then
  fail "desktop DML unit test failed; see /tmp/prosenjitos_desktop_unit_test.log"
else
  note "desktop DML host unit tests passed"
fi
if ! make clean >/tmp/prosenjitos_make_clean.log 2>&1; then fail "make clean failed; see /tmp/prosenjitos_make_clean.log"; fi
if ! make iso >/tmp/prosenjitos_make_iso.log 2>&1; then fail "make iso failed; see /tmp/prosenjitos_make_iso.log"; fi

echo "[5/6] QEMU smoke test"
if [[ -f build/prosenjitOS.iso ]]; then
  rm -f build/desktop-qemu-serial.log build/desktop-qemu-debug.log
  qemu_rc=0
  set +e
  timeout 20s qemu-system-x86_64 -display none -serial file:build/desktop-qemu-serial.log -no-reboot -no-shutdown -cdrom build/prosenjitOS.iso -d int -D build/desktop-qemu-debug.log >/tmp/prosenjitos_qemu_stdout.log 2>/tmp/prosenjitos_qemu_stderr.log
  qemu_rc=$?
  set -e
  case "$qemu_rc" in
    0|124) note "QEMU smoke window ended with status $qemu_rc (timeout is expected for a running kernel)" ;;
    *) fail "QEMU exited with status $qemu_rc; inspect serial/debug logs" ;;
  esac
  if [[ -s build/desktop-qemu-serial.log ]]; then
    note "QEMU serial log generated"
    for marker in \
      "ProsenjitOS: x86-64 long mode active" \
      "ProsenjitOS: CPL3 user-mode and IPC syscall test passed" \
      "ProsenjitOS: process lifecycle/parent/exit-wait test passed" \
      "ProsenjitOS: ProsenjitFS format/mount/allocation/journal/recovery self-test passed"; do
      if grep -Fq "$marker" build/desktop-qemu-serial.log; then
        note "QEMU milestone verified: $marker"
      else
        fail "QEMU serial log missing required milestone: $marker"
      fi
    done
  else
    fail "QEMU produced no serial log"
  fi
else
  fail "build/prosenjitOS.iso was not produced"
fi

echo "[6/6] Report"
{
  echo "# ProsenjitOS Desktop Prerequisite Integration Gate"
  echo
  echo "Generated: $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
  echo
  echo "## Rules"
  echo "- Desktop environment is userspace."
  echo "- Existing Xorg/X11 or Wayland and XFCE are integration targets."
  echo "- No desktop stack is added to kernel-core."
  echo "- This gate verifies prerequisites only; it does not prove a running GUI, input route, or XFCE session."
  echo "- GitHub-hosted Actions are not required."
  echo
  echo "## Notes"
  for n in "${notes[@]}"; do echo "- $n"; done
  echo
  echo "## Blockers"
  if ((failure_count > 0)); then
    for f in "${failures[@]}"; do echo "- $f"; done
  else
    echo "none"
  fi
} > "$REPORT"

if ((failure_count > 0)); then
  echo "DESKTOP PREREQUISITE GATE: NOT PASS"
  for f in "${failures[@]}"; do echo " - $f"; done
  echo "Report: $REPORT"
  exit 1
fi
echo "DESKTOP PREREQUISITE GATE: PASS (GUI runtime remains unverified)"
echo "Report: $REPORT"
