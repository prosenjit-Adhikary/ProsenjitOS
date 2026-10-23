#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
STAGE1="$ROOT/boot/native_stage1.asm"
STAGE2="$ROOT/boot/native_stage2.asm"

fail() {
  echo "FAIL: native E820 boot safety: $*" >&2
  exit 1
}

require_literal() {
  local literal="$1" message="$2"
  if ! grep -F "$literal" "$STAGE2" >/dev/null; then
    fail "$message"
  fi
}

require_stage1_literal() {
  local literal="$1" message="$2"
  if ! grep -F "$literal" "$STAGE1" >/dev/null; then
    fail "$message"
  fi
}

# A bounded, complete E820 map is required before the allocator sees RAM ranges.
require_literal 'mov dword [es:di + 20], 1' "must request E820 extended attributes"
require_literal 'push ds' "must preserve DS across the BIOS call"
require_literal 'push es' "must preserve ES across the BIOS call"
require_literal 'push di' "must preserve the destination pointer across the BIOS call"
require_literal 'push bp' "must preserve the E820 entry counter across the BIOS call"
require_literal 'pop bp' "must restore the E820 entry counter"
require_literal 'cmp ecx, 20' "must reject undersized E820 responses"
require_literal 'cmp ecx, 24' "must distinguish legacy and extended E820 responses"
require_literal 'test dword [es:di + 20], 1' "must skip disabled extended E820 entries"
require_literal 'mov dword [es:di + 20], 0' "must clear the Multiboot2 reserved dword"
require_literal 'jc .fail' "must fail closed on E820 BIOS errors"
# EDD reads must complete every requested sector before execution/copy.
require_stage1_literal 'cmp word [dap_count], STAGE2_SECTORS' "stage 1 must reject short stage-2 reads"
require_literal 'cmp word [dap_count], ax' "stage 2 must reject short kernel reads"
require_stage1_literal 'jne disk_error' "stage 1 short reads must enter the disk-error halt path"
require_literal 'jne .fail' "stage 2 short reads must enter the fatal boot path"
require_literal 'cmp bp, 32' "must enforce the E820 entry-buffer capacity"
require_literal 'jae .fail' "must reject truncated E820 maps"
require_literal 'test ebx, ebx' "must follow the E820 continuation token"

# Do not reintroduce a partial-map success path on carry or buffer exhaustion.
e820_block="$(sed -n '/^collect_e820:/,/^build_mb2_info:/p' "$STAGE2")"
if grep -F 'jc .done' <<<"$e820_block" >/dev/null; then
  fail "E820 carry must not be accepted as successful end-of-map"
fi

echo "PASS: native E820 extended attributes, register preservation, and fail-closed bounds"
