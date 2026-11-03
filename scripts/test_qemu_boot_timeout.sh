#!/usr/bin/env bash
set -euo pipefail

workflow_file="${1:-.github/workflows/build.yml}"
if [[ ! -f "$workflow_file" ]]; then
  echo "::error::Build workflow not found: $workflow_file"
  exit 1
fi

mapfile -t timeout_values < <(
  grep -Eo 'timeout (-k [0-9]+s )?[0-9]+s qemu-system-x86_64' "$workflow_file" |
    sed -E 's/timeout (-k [0-9]+s )?([0-9]+)s.*/\2/'
)

if [[ "${#timeout_values[@]}" -ne 2 ]]; then
  echo "::error::Expected exactly two QEMU boot-test commands (ISO and native BIOS); found ${#timeout_values[@]}"
  exit 1
fi

for seconds in "${timeout_values[@]}"; do
  if (( seconds < 30 )); then
    echo "::error::QEMU boot-test timeout must be at least 30 seconds; found ${seconds}s"
    exit 1
  fi
done

echo "QEMU boot timeout policy test passed (ISO and native BIOS each have at least 30 seconds)."
