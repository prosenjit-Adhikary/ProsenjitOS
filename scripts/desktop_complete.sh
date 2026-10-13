#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(git rev-parse --show-toplevel 2>/dev/null || true)"
if [[ -z "$ROOT" ]]; then
  echo "ERROR: run this script from inside the ProsenjitOS repository." >&2
  exit 2
fi
cd "$ROOT"

REPORT="reports/desktop-integration-report.md"
mkdir -p reports
failures=()
notes=()
failure_count=0
note() { notes+=("$1"); }
fail() { failures+=("$1"); failure_count=$((failure_count + 1)); }
need_cmd() { command -v "$1" >/dev/null 2>&1 || fail "missing command: $1"; }

echo "[1/7] Kernel/userspace architecture boundary"
if grep -RInE '(^|[^[:alnum:]_])(XFCE|Xorg|Wayland|X11)([^[:alnum:]_]|$)' kernel \
  --include='*.c' --include='*.h' --include='*.asm' --include='*.S' \
  > /tmp/prosenjitos_kernel_gui_hits 2>/dev/null; then
  fail "desktop-stack reference detected inside kernel sources; inspect /tmp/prosenjitos_kernel_gui_hits"
else
  note "kernel GUI-stack contamination check passed"
fi

echo "[2/7] Build prerequisites"
for c in git make gcc ld nasm grub-file grub-mkrescue xorriso objcopy dd truncate stat qemu-system-x86_64 timeout grep find; do
  need_cmd "$c"
done

echo "[3/7] Kernel foundations (necessary, not sufficient)"
if [[ -f kernel/elf_loader.c ]] && grep -Eq 'pros_elf_(validate|load)' kernel/elf_loader.c; then
  note "kernel ELF loader implementation exists"
else
  fail "kernel ELF loader implementation not found"
fi
if [[ -f kernel/service_manager.c ]] && grep -Eq 'service_manager_init|service_start_all' kernel/service_manager.c; then
  note "kernel service-management primitive exists; this is not a userspace init process"
else
  fail "kernel service-management primitive not found"
fi
if [[ -f include/abi/display.h && -f include/abi/input.h ]]; then
  note "versioned display/input ABI header files exist"
else
  fail "display/input ABI headers are missing"
fi
if [[ -f kernel/display.c ]] && grep -Eq 'pros_display_(get_info|present)' kernel/display.c; then
  note "kernel display backend boundary exists; a mock backend is not hardware support"
else
  fail "kernel display backend boundary not found"
fi

echo "[4/7] Actual userspace desktop implementation"
user_sources="$(find user -type f \( -name '*.c' -o -name '*.h' -o -name '*.S' -o -name '*.asm' \) ! -name 'README.md' -print 2>/dev/null || true)"
if [[ -n "$user_sources" ]]; then
  note "userspace source files found:"
  while IFS= read -r f; do note "  $f"; done <<< "$user_sources"
else
  fail "no actual userspace program/service source exists under user/ (README and ABI declarations do not count)"
fi
if ! grep -RIlE 'PROS_SYSCALL_(DISPLAY|INPUT)|SYS_(DISPLAY|INPUT)|display_(get_info|present)|input_(read|poll)' kernel user include --include='*.c' --include='*.h' --include='*.S' --include='*.asm' >/dev/null 2>&1; then
  fail "no display/input syscall or userspace-facing dispatch path detected"
else
  note "display/input syscall or dispatch symbols detected; verify their implementation and authorization"
fi
if ! find user -type f \( -iname '*init*' -o -iname '*session*' -o -iname '*desktop*' \) -print 2>/dev/null | grep -q .; then
  fail "userspace init/session/desktop launcher source is missing"
else
  note "userspace init/session/desktop-related source path detected"
fi
if ! grep -RIlE 'Xorg|Wayland|XFCE|xfce4-session' user ports vendor third_party --include='Makefile' --include='*.mk' --include='*.sh' --include='*.conf' 2>/dev/null | grep -q .; then
  fail "no explicit userspace window-system/desktop integration or build manifest detected"
else
  note "window-system/desktop integration references detected; runtime boot still must be demonstrated"
fi

echo "[5/7] Warnings-as-errors build"
if command -v make >/dev/null 2>&1; then
  if ! make clean > /tmp/prosenjitos_make_clean.log 2>&1; then
    fail "make clean failed; see /tmp/prosenjitos_make_clean.log"
  fi
  if ! make iso > /tmp/prosenjitos_make_iso.log 2>&1; then
    fail "make iso failed; see /tmp/prosenjitos_make_iso.log"
  else
    note "kernel ISO and native BIOS artifacts built"
  fi
fi

echo "[6/7] QEMU base-kernel boot smoke test"
if [[ -f build/prosenjitOS.iso ]] && command -v qemu-system-x86_64 >/dev/null 2>&1 && command -v timeout >/dev/null 2>&1; then
  rm -f build/desktop-qemu-serial.log build/desktop-qemu-debug.log
  set +e
  timeout 20s qemu-system-x86_64 \
    -display none \
    -serial file:build/desktop-qemu-serial.log \
    -no-reboot -no-shutdown \
    -cdrom build/prosenjitOS.iso \
    -d int -D build/desktop-qemu-debug.log \
    >/tmp/prosenjitos_qemu_stdout.log 2>/tmp/prosenjitos_qemu_stderr.log
  qemu_rc=$?
  set -e
  case "$qemu_rc" in
    0) note "QEMU exited normally" ;;
    124) note "QEMU reached the 20-second smoke-test limit (expected for a kernel that remains running)" ;;
    *) fail "QEMU exited with status $qemu_rc; inspect build/desktop-qemu-debug.log and /tmp/prosenjitos_qemu_stderr.log" ;;
  esac
  if [[ -s build/desktop-qemu-serial.log ]]; then
    note "QEMU serial log generated"
    for marker in \
      "ProsenjitOS: x86-64 long mode active" \
      "ProsenjitOS: CPL3 user-mode and IPC syscall test passed"; do
      if grep -Fq "$marker" build/desktop-qemu-serial.log; then
        note "boot marker present: $marker"
      else
        fail "missing required base-kernel boot marker: $marker"
      fi
    done
  else
    fail "QEMU produced no serial log"
  fi
else
  fail "build/prosenjitOS.iso or QEMU is unavailable; base-kernel boot test was not run"
fi

echo "[7/7] Report"
{
  echo "# ProsenjitOS Desktop Integration Gate"
  echo
  echo "Generated: $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
  echo
  echo "## Scope"
  echo "- This gate checks the kernel foundations, build/boot health, and evidence of real userspace desktop integration."
  echo "- A passing kernel build or mock display self-test is not equivalent to a working graphical desktop."
  echo "- The desktop stack must remain in userspace; kernel-core must not contain XFCE, Xorg/X11, or Wayland."
  echo "- Existing Xorg/X11 or Wayland and XFCE are integration targets, not currently assumed to work."
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
  echo "DESKTOP GATE: NOT COMPLETE ($failure_count blocker(s))"
  for f in "${failures[@]}"; do echo " - $f"; done
  echo "Report: $REPORT"
  exit 1
fi

echo "DESKTOP GATE: PASS"
echo "Report: $REPORT"
