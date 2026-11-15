#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CLASSIFIER="$ROOT/scripts/classify_ci_changes.sh"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

assert_value() {
  local output_file="$1" key="$2" expected="$3"
  if ! grep -Fxq "$key=$expected" "$output_file"; then
    echo "::error::Expected $key=$expected; got: $(grep -E "^$key=" "$output_file" || echo missing)" >&2
    return 1
  fi
}

run_case() {
  local name="$1" path="$2"
  shift 2
  local paths="$TMP/$name.paths" output="$TMP/$name.output"
  : > "$output"
  printf '%s\n' "$path" > "$paths"
  bash "$CLASSIFIER" "$paths" "$output"
  while (( $# >= 2 )); do
    assert_value "$output" "$1" "$2"
    shift 2
  done
}

run_case docs-only docs/UNIFIED-OS-ARCHITECTURE.md \
  doc_check true kernel_build false iso_build false iso_boot false native_build false native_boot false

run_case kernel-runtime kernel/heap.c \
  kernel_build true iso_build true iso_boot true native_build false native_boot false

run_case shared-storage-header kernel/include/block.h \
  kernel_build true iso_build true iso_boot true native_build true native_boot true fs_diag true

run_case fatfs-vendor third_party/fatfs/option/ffunicode.c \
  kernel_build true iso_build true iso_boot true native_build true native_boot true

run_case build-orchestration scripts/build.sh \
  kernel_build true iso_build true native_build true iso_boot false native_boot false

run_case shared-entry boot/entry.asm \
  kernel_build true iso_build true iso_boot true native_build true native_boot true timer_tests true

run_case native-loader boot/native_stage2.asm \
  kernel_build true native_build true native_boot true iso_build false iso_boot false

run_case desktop-unit user/desktop/desktop_manager.c \
  desktop_tests true kernel_build false iso_boot false

run_case scheduler-abi kernel/include/ipc.h \
  timer_tests true kernel_build true iso_boot true

run_case qemu-timeout-policy scripts/test_qemu_boot_timeout.sh \
  qemu_policy_tests true kernel_build false iso_boot false

run_case workflow-change .github/workflows/build.yml \
  ci_mapping_tests true qemu_policy_tests true kernel_build true desktop_tests true security_service_tests true timer_tests true fs_diag true iso_build true iso_boot true native_build true native_boot true

echo "ProsenjitOS CI changed-path dependency mapping tests passed."
