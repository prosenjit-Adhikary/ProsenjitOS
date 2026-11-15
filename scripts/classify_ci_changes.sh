#!/usr/bin/env bash
set -euo pipefail

# Classify changed repository paths into the validation contracts that exercise
# them. Input is a newline-delimited path file; output is GitHub key=value data.
if [[ $# -ne 2 ]]; then
  echo "Usage: $0 <changed-paths-file> <github-output-file>" >&2
  exit 2
fi
changed_paths_file="$1"
output_file="$2"
if [[ ! -f "$changed_paths_file" ]]; then
  echo "Changed-path input does not exist: $changed_paths_file" >&2
  exit 2
fi

doc_check=false
kernel_build=false
desktop_tests=false
security_service_tests=false
timer_tests=false
fs_diag=false
iso_build=false
iso_boot=false
native_build=false
native_boot=false
ci_mapping_tests=false
qemu_policy_tests=false
full_matrix=false

while IFS= read -r path || [[ -n "$path" ]]; do
  [[ -n "$path" ]] || continue

  case "$path" in
    *.md|*.markdown)
      doc_check=true
      ;;
  esac

  # Changing the selector or any workflow can invalidate the test-selection
  # policy itself. Fail safe: run every implemented gate and test the selector.
  case "$path" in
    .github/workflows/*|.github/actions/*|scripts/classify_ci_changes.sh|scripts/test_ci_change_mapping.sh)
      full_matrix=true
      ci_mapping_tests=true
      qemu_policy_tests=true
      ;;
  esac

  # Every kernel translation unit, kernel-private header, shared ABI header,
  # and vendored FatFs source/header is compiled into or consumed by the kernel.
  # The ISO QEMU integration boot exercises the broad kernel self-test suite.
  case "$path" in
    kernel/*|kernel/include/*|include/*|third_party/fatfs/*)
      kernel_build=true
      iso_build=true
      iso_boot=true
      ;;
  esac

  # GRUB configuration is an ISO boot contract; common entry/header code is
  # shared by GRUB and native BIOS and must be tested through both paths.
  case "$path" in
    boot/grub.cfg)
      kernel_build=true
      iso_build=true
      iso_boot=true
      ;;
    boot/entry.asm|boot/multiboot2_header.asm)
      kernel_build=true
      iso_build=true
      iso_boot=true
      native_build=true
      native_boot=true
      timer_tests=true
      ;;
    boot/native_*)
      kernel_build=true
      native_build=true
      native_boot=true
      ;;
  esac

  # Build orchestration changes validate both artifact builders, but do not
  # boot unchanged runtime/loader code.
  case "$path" in
    scripts/create_usb_dual_fs_image.py)
      kernel_build=true
      iso_build=true
      iso_boot=true
      ;;
    scripts/build.sh|Makefile)
      kernel_build=true
      iso_build=true
      native_build=true
      ;;
  esac

  # Native BIOS QEMU also verifies the real-disk, partition, VFS and filesystem
  # contracts. These are dependencies of the native persistent-mount path.
  case "$path" in
    kernel/linker.ld|kernel/ata_pio.c|kernel/block.c|kernel/storage.c|kernel/partition.c|kernel/prosenjitfs.c|kernel/fatfs_*.c|kernel/vfs*.c|kernel/include/ata_pio.h|kernel/include/block.h|kernel/include/partition.h|kernel/include/prosenjitfs.h|kernel/include/storage.h|kernel/include/fatfs_*.h|kernel/include/vfs.h|include/prosenjitfs*.h|include/prosenjitfs_format.h|include/vfs_provider.h|third_party/fatfs/*)
      kernel_build=true
      iso_build=true
      iso_boot=true
      native_build=true
      native_boot=true
      fs_diag=true
      ;;
  esac

  # The native image formatter is built and consumed only by the native
  # disk-image path; it does not alter the ISO kernel/runtime contract.
  case "$path" in
    tools/mkprosenjitfs.c)
      kernel_build=true
      native_build=true
      native_boot=true
      ;;
  esac

  # Host-side tests are selected only for the code and fixtures they compile
  # or inspect. The timer/stack scripts also inspect scheduler, IPC, syscall,
  # interrupt-frame and boot-entry source contracts.
  case "$path" in
    user/desktop/*|tests/desktop_manager_test.c|scripts/desktop_complete.sh|scripts/test_desktop_manager.sh)
      desktop_tests=true
      ;;
    user/security/*|tests/security_control_center_service_test.c|scripts/test_security_control_center_service.sh)
      security_service_tests=true
      ;;
    kernel/idt.asm|kernel/context_switch.asm|kernel/syscall.c|kernel/thread.c|kernel/ipc.c|kernel/kernel.c|kernel/paging.c|kernel/timer.c|kernel/include/thread.h|kernel/include/ipc.h|kernel/include/timer.h|boot/entry.asm|scripts/test_timer_irq_assembly.sh|scripts/test_kernel_stack_guard.sh)
      timer_tests=true
      ;;
    scripts/test_qemu_boot_timeout.sh)
      qemu_policy_tests=true
      ;;
  esac
done < "$changed_paths_file"

if [[ "$full_matrix" == true ]]; then
  kernel_build=true
  desktop_tests=true
  security_service_tests=true
  timer_tests=true
  fs_diag=true
  iso_build=true
  iso_boot=true
  native_build=true
  native_boot=true
fi

{
  echo "doc_check=$doc_check"
  echo "kernel_build=$kernel_build"
  echo "desktop_tests=$desktop_tests"
  echo "security_service_tests=$security_service_tests"
  echo "timer_tests=$timer_tests"
  echo "fs_diag=$fs_diag"
  echo "iso_build=$iso_build"
  echo "iso_boot=$iso_boot"
  echo "native_build=$native_build"
  echo "native_boot=$native_boot"
  echo "ci_mapping_tests=$ci_mapping_tests"
  echo "qemu_policy_tests=$qemu_policy_tests"
} >> "$output_file"

echo "Selected checks: docs=$doc_check; kernel build=$kernel_build; desktop=$desktop_tests; security-service=$security_service_tests; timer/IRQ=$timer_tests; filesystem diagnostics=$fs_diag; ISO build/boot=$iso_build/$iso_boot; native BIOS build/boot=$native_build/$native_boot; CI mapping=$ci_mapping_tests; QEMU timeout policy=$qemu_policy_tests"
