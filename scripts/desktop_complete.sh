#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(git rev-parse --show-toplevel 2>/dev/null || true)"
if [[ -z "$ROOT" ]]; then
  echo "ERROR: run from the ProsenjitOS repository." >&2
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

echo "[1/7] Kernel/userspace boundary"
if grep -RInE '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"][^>"]*(X11|Xorg|wayland|xfce)|\b(XOpenDisplay|wl_display_connect|xfce4-session)\b' \
    kernel --include='*.c' --include='*.h' --include='*.asm' --include='*.S' \
    >/tmp/prosenjitos_kernel_gui_hits 2>/dev/null; then
  fail "desktop-stack include/API reference detected in kernel source; inspect /tmp/prosenjitos_kernel_gui_hits"
else
  note "No desktop-stack includes/API calls detected in kernel source"
fi

echo "[2/7] Local build prerequisites"
for c in git make gcc ld nasm qemu-system-x86_64 timeout grep; do need_cmd "$c"; done

echo "[3/7] ELF loader and kernel user-entry foundation"
if [[ -f kernel/elf_loader.c ]] &&
   grep -qE '^int pros_elf_validate\(' kernel/elf_loader.c &&
   grep -qE '^int pros_elf_load_with_args\(' kernel/elf_loader.c &&
   grep -qE '^int pros_elf_selftest\(' kernel/elf_loader.c; then
  note "Kernel ELF loader source and self-test entry point exist (not proof of a general userspace runtime)"
else
  fail "kernel ELF loader implementation/self-test source is missing"
fi

if [[ -f kernel/kernel.c ]] &&
   grep -qF 'enter_user_mode(elf_result.entry, elf_result.stack_top, user_test_handle);' kernel/kernel.c; then
  note "Kernel has a CPL3 entry path for its built-in user-mode test"
else
  fail "CPL3 user-mode entry call was not detected"
fi

echo "[4/7] Init/service infrastructure"
if [[ -f kernel/service_manager.c ]] &&
   grep -qE '^int service_manager_selftest\(' kernel/service_manager.c &&
   grep -qE '^int service_start_all\(' kernel/service_manager.c; then
  note "Kernel-side service dependency manager and self-test source exist"
else
  fail "kernel service manager implementation/self-test source is missing"
fi

if [[ -f kernel/init.c ]] &&
   grep -qE '^int pros_init_load\(' kernel/init.c &&
   grep -qF 'pros_elf_load_with_args(process, image, length,' kernel/init.c; then
  note "Filesystem-backed /init loader constructs an initial argc/envp stack; live boot integration is still required"
else
  fail "filesystem-backed init loader or argument-aware initial-stack construction is missing"
fi

# A kernel-side loader/service registry is not equivalent to a userspace PID 1.
if [[ ! -f user/init.c && ! -f user/init/main.c && ! -f user/init/Makefile ]]; then
  fail "no userspace init/PID 1 source/build target detected; current service manager is kernel-side"
fi

# Ordinary userspace needs a complete minimum path, not just one syscall
# symbol: file open/read/write plus process launch. Keep these as separate
# gates so a partial ABI cannot accidentally count as sufficient.
for call in OPEN READ WRITE; do
  if grep -qF "PROSENJIT_SYS_${call}" include/abi/syscall.h &&
     grep -qF "case PROSENJIT_SYS_${call}:" kernel/syscall.c; then
    note "Public ABI and kernel dispatcher both define ${call}"
  else
    fail "file syscall ${call} is missing from the public ABI or kernel dispatcher"
  fi
done

process_launch_found=0
for call in EXEC SPAWN PROCESS_CREATE; do
  if grep -qF "PROSENJIT_SYS_${call}" include/abi/syscall.h &&
     grep -qF "case PROSENJIT_SYS_${call}:" kernel/syscall.c; then
    process_launch_found=1
    note "Public ABI and kernel dispatcher define process launch via ${call}"
    break
  fi
done
if ((process_launch_found == 0)); then
  fail "public ABI/kernel dispatcher lack EXEC, SPAWN or PROCESS_CREATE; init cannot launch services or a desktop session"
fi

# The boot path currently constructs an embedded test ELF. Require the
# filesystem-backed /init loader to be called from the real boot path before
# treating it as normal userspace startup.
if grep -qF 'pros_init_load(' kernel/kernel.c; then
  note "Normal boot path references filesystem-backed init loading"
else
  fail "pros_init_load() is not wired into kernel/kernel.c; boot still uses the embedded CPL3 test image"
fi

echo "[5/7] Real display/input path and desktop runtime"
if [[ -f kernel/display.c ]] &&
   grep -qE '^int pros_display_bind\(' kernel/display.c &&
   grep -qE '^int pros_display_selftest\(' kernel/display.c &&
   grep -qF 'static int display_info_valid(' kernel/display.c &&
   grep -qF 'return pros_driver_unbind(id);' kernel/display.c &&
   grep -qF 'PROS_DISPLAY_FORMAT_XRGB8888 1u' kernel/include/display.h &&
   grep -qF 'PROS_DISPLAY_XRGB8888 = 1' include/abi/display.h &&
   grep -qF 'framebuffer_bytes' include/abi/display.h &&
   grep -qF 'permission-checked mapping/handle' include/abi/display.h &&
   grep -qF 'sizeof(struct pros_display_info) == 40u' include/abi/display.h &&
   grep -qF 'sizeof(struct pros_display_event) == 32u' include/abi/display.h &&
   grep -qF 'sizeof(struct pros_input_event) == 32u' include/abi/input.h &&
   grep -qF 'uint32_t flags,reserved;' include/abi/input.h; then
  note "Display/input ABI layouts are explicitly sized and checked at compile time; display security and lifecycle contract is present"
else
  fail "display binding validation/lifecycle, self-tests or public/internal ABI contract is missing/inconsistent"
fi

# The current display self-test backend is synthetic; it does not bind a real
# framebuffer device or expose a userspace mapping/event queue.
display_backend_candidates=/tmp/prosenjitos_display_backend_candidates
display_backend_hits=/tmp/prosenjitos_display_backend_hits
: > "$display_backend_candidates"
: > "$display_backend_hits"
grep -RIlE 'PROS_DISPLAY_BACKEND_MAGIC|pros_display_backend' drivers kernel \
  --include='*.c' --include='*.h' 2>/dev/null > "$display_backend_candidates" || true
grep -vE '^kernel/display\.c$|^kernel/include/display[.]h$' \
  "$display_backend_candidates" > "$display_backend_hits" || true
if [[ ! -s "$display_backend_hits" ]]; then
  fail "no separate real display backend implementation detected (mock display self-test is not hardware support)"
else
  note "Separate display backend source detected; hardware/runtime operation still requires testing"
fi

if [[ ! -f drivers/ps2_keyboard.c && ! -f drivers/usb_hid.c &&
      ! -f kernel/ps2_keyboard.c && ! -f kernel/usb_hid.c ]]; then
  fail "no PS/2 or USB-HID keyboard/pointer driver detected; include/abi/input.h alone is only a contract"
fi

if [[ ! -f user/Makefile && ! -f user/build.sh ]]; then
  fail "no userspace build/package target detected for an external Xorg/Wayland + XFCE stack"
fi
if [[ ! -d user/xfce && ! -d user/desktop && ! -d user/session ]]; then
  fail "no desktop session integration/configuration detected for XFCE or an existing window system"
fi

echo "[6/7] Clean local build and QEMU kernel/user-entry smoke test"
for c in make; do need_cmd "$c"; done
if make clean >/tmp/prosenjitos_make_clean.log 2>&1; then
  note "make clean passed"
else
  fail "make clean failed; see /tmp/prosenjitos_make_clean.log"
fi

if make iso >/tmp/prosenjitos_make_iso.log 2>&1; then
  note "make iso passed with repository warning/error policy"
else
  fail "make iso failed; see /tmp/prosenjitos_make_iso.log"
fi

if [[ -f build/prosenjitOS.iso ]]; then
  rm -f build/desktop-qemu-serial.log build/desktop-qemu-debug.log
  qemu_rc=0
  set +e
  timeout 20s qemu-system-x86_64 -display none \
    -serial file:build/desktop-qemu-serial.log -no-reboot -no-shutdown \
    -cdrom build/prosenjitOS.iso -d int -D build/desktop-qemu-debug.log \
    >/tmp/prosenjitos_qemu_stdout.log 2>/tmp/prosenjitos_qemu_stderr.log
  qemu_rc=$?
  set -e

  if [[ -s build/desktop-qemu-serial.log ]]; then
    note "QEMU serial log generated"
  else
    fail "QEMU produced no serial log"
  fi

  # The kernel is designed to remain running; timeout(124) is expected only
  # when the serial trace proves that it reached the CPL3 entry checkpoint.
  if grep -qF 'ProsenjitOS: entering CPL3 user-mode syscall test' build/desktop-qemu-serial.log 2>/dev/null; then
    note "QEMU reached the CPL3 user-mode entry checkpoint"
    if [[ "$qemu_rc" == "124" ]]; then
      note "QEMU remained running until the 20-second timeout (expected for a non-shutdown kernel)"
    elif [[ "$qemu_rc" == "0" ]]; then
      note "QEMU exited normally after reaching the user-entry checkpoint"
    else
      fail "QEMU exited with status $qemu_rc after the user-entry checkpoint; inspect build/desktop-qemu-debug.log and /tmp/prosenjitos_qemu_stderr.log"
    fi
  else
    fail "QEMU did not reach the CPL3 entry checkpoint; inspect build/desktop-qemu-serial.log and build/desktop-qemu-debug.log (QEMU status $qemu_rc)"
  fi

  if grep -qF 'ProsenjitOS: ERROR' build/desktop-qemu-serial.log 2>/dev/null; then
    fail "kernel emitted an ERROR marker; inspect build/desktop-qemu-serial.log"
  fi
else
  fail "build/prosenjitOS.iso was not produced"
fi

echo "[7/7] Report"
{
  echo "# ProsenjitOS Desktop Integration Gate"
  echo
  echo "Generated: $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
  echo
  echo "## Scope"
  echo "- This script validates source/build/kernel user-entry foundations; it does not itself implement a desktop."
  echo "- QEMU timeout is accepted only if the serial log reaches the CPL3 entry checkpoint."
  echo "- A successful kernel/user-entry smoke test is not a successful graphical desktop boot."
  echo "- Prefer local Debian/self-hosted verification; no GitHub-hosted Actions dependency is required."
  echo
  echo "## Passed checks / notes"
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
  echo "DESKTOP GATE: NOT COMPLETE"
  for f in "${failures[@]}"; do echo " - $f"; done
  echo "Report: $REPORT"
  exit 1
fi

echo "DESKTOP GATE: PASS"
echo "Report: $REPORT"
