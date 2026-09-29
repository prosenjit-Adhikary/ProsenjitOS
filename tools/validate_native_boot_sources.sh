#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD=${BUILD_DIR:-"$ROOT/build/native-source-check"}
STAGE2_SECTORS=32
KERNEL_LBA=33
MAX_KERNEL_SECTORS=1536
STAGE2_LOAD_BASE=0x8000
STAGE2_LOAD_BYTES=$((STAGE2_SECTORS * 512))
STAGE2_LOAD_END=$((STAGE2_LOAD_BASE + STAGE2_LOAD_BYTES))
STACK_POINTER=0xF000

mkdir -p "$BUILD"

command -v nasm >/dev/null 2>&1 || {
    echo "nasm is required for native boot source validation" >&2
    exit 1
}

nasm -f bin -dSTAGE2_SECTORS="$STAGE2_SECTORS"     "$ROOT/boot/native_stage1.asm" -o "$BUILD/stage1.bin"

nasm -f bin -dKERNEL_LBA="$KERNEL_LBA" -dKERNEL_SECTORS="$MAX_KERNEL_SECTORS"     "$ROOT/boot/native_stage2.asm" -o "$BUILD/stage2-max.bin"

stage1_size=$(wc -c < "$BUILD/stage1.bin")
stage2_size=$(wc -c < "$BUILD/stage2-max.bin")

[ "$stage1_size" -eq 512 ] || {
    echo "stage-1 source does not assemble to exactly 512 bytes" >&2
    exit 1
}
[ "$stage2_size" -le $((STAGE2_SECTORS * 512)) ] || {
    echo "stage-2 source exceeds its reserved 32-sector area" >&2
    exit 1
}

signature=$(od -An -tx1 -j 510 -N 2 "$BUILD/stage1.bin" | tr -d ' 
')
[ "$signature" = "55aa" ] || {
    echo "stage-1 source is missing BIOS signature 0xAA55" >&2
    exit 1
}

# Keep the fixed native layout constants explicit and internally consistent.
[ "$KERNEL_LBA" -eq $((1 + STAGE2_SECTORS)) ] || {
    echo "native kernel LBA is inconsistent with stage-2 reservation" >&2
    exit 1
}

[ "$((STACK_POINTER))" -ge "$STAGE2_LOAD_END" ] || {
    echo "native stack overlaps the stage-2 load area" >&2
    exit 1
}

echo "Native boot source validation passed"
echo "  stage-1 bytes: $stage1_size"
echo "  stage-2 bytes: $stage2_size"
echo "  kernel LBA:    $KERNEL_LBA"
echo "  max sectors:   $MAX_KERNEL_SECTORS"
echo "  stack pointer: 0x$(printf "%X" "$STACK_POINTER")"
