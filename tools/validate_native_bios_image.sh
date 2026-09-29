#!/bin/sh
set -eu

IMAGE=${1:-build/prosenjit-native-bios.img}
STAGE1=${2:-build/native_stage1.bin}
STAGE2=${3:-build/native_stage2.bin}
KERNEL=${4:-build/prosenjit.bin}

STAGE2_SECTORS=32
KERNEL_LBA=33
SECTOR_SIZE=512
MAX_KERNEL_SECTORS=1536

for f in "$IMAGE" "$STAGE1" "$STAGE2" "$KERNEL"; do
    if [ ! -f "$f" ]; then
        echo "missing native boot artifact: $f" >&2
        exit 1
    fi
done

stage1_size=$(wc -c < "$STAGE1")
stage2_size=$(wc -c < "$STAGE2")
kernel_size=$(wc -c < "$KERNEL")
image_size=$(wc -c < "$IMAGE")

[ "$stage1_size" -eq 512 ] || { echo "stage-1 must be exactly 512 bytes" >&2; exit 1; }
[ "$stage2_size" -le $((STAGE2_SECTORS * SECTOR_SIZE)) ] || {
    echo "stage-2 exceeds reserved area" >&2
    exit 1
}
[ "$kernel_size" -gt 0 ] || { echo "kernel image is empty" >&2; exit 1; }

kernel_sectors=$(( (kernel_size + SECTOR_SIZE - 1) / SECTOR_SIZE ))
[ "$kernel_sectors" -le "$MAX_KERNEL_SECTORS" ] || {
    echo "kernel exceeds native loader staging limit" >&2
    exit 1
}

expected_image_size=$(( (KERNEL_LBA + kernel_sectors) * SECTOR_SIZE ))
[ "$image_size" -eq "$expected_image_size" ] || {
    echo "native image size does not match its fixed LBA layout" >&2
    echo "expected=$expected_image_size actual=$image_size" >&2
    exit 1
}

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

dd if="$IMAGE" of="$tmpdir/stage1.bin" bs="$SECTOR_SIZE" count=1 status=none
dd if="$IMAGE" of="$tmpdir/stage2.bin" bs="$SECTOR_SIZE" skip=1 count="$STAGE2_SECTORS" status=none
dd if="$IMAGE" of="$tmpdir/kernel.bin" bs="$SECTOR_SIZE" skip="$KERNEL_LBA" count="$kernel_sectors" status=none

cmp "$STAGE1" "$tmpdir/stage1.bin"

# The reserved stage-2 area is padded with zeroes after the assembled image.
if ! cmp "$STAGE2" "$tmpdir/stage2.bin" >/dev/null 2>&1; then
    echo "stage-2 payload does not match native image" >&2
    exit 1
fi

# The final sector may contain zero padding when the kernel is not 512-byte aligned.
if ! cmp "$KERNEL" "$tmpdir/kernel.bin" >/dev/null 2>&1; then
    echo "kernel payload does not match native image" >&2
    exit 1
fi

signature=$(od -An -tx1 -j 510 -N 2 "$IMAGE" | tr -d ' \n')
[ "$signature" = "55aa" ] || {
    echo "native BIOS boot signature 0xAA55 is missing" >&2
    exit 1
}

echo "Native BIOS layout validation passed"
echo "  stage-1:  LBA 0"
echo "  stage-2:  LBA 1..$STAGE2_SECTORS"
echo "  kernel:   LBA $KERNEL_LBA..$((KERNEL_LBA + kernel_sectors - 1))"
