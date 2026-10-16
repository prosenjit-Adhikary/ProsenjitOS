#!/usr/bin/env python3
"""Read-only validator for the disposable native BIOS disk image layout.

This tool never opens an image for writing. It validates the MBR signature,
the single expected ProsenjitFS partition, image bounds, and that the fixed
kernel payload does not overlap the partition.
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

SECTOR_SIZE = 512
EXPECTED_START_LBA = 2048
EXPECTED_SECTORS = 512
EXPECTED_TYPE = 0xDA
MBR_SIGNATURE = b"\x55\xAA"


def fail(message: str) -> int:
    print(f"native disk layout: ERROR: {message}", file=sys.stderr)
    return 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path, help="native BIOS raw disk image")
    parser.add_argument("--kernel-end-lba", type=int, required=True,
                        help="exclusive end LBA of the kernel payload")
    parser.add_argument("--partition-start-lba", type=int,
                        default=EXPECTED_START_LBA)
    parser.add_argument("--partition-sectors", type=int,
                        default=EXPECTED_SECTORS)
    args = parser.parse_args()

    try:
        size = args.image.stat().st_size
        with args.image.open("rb") as stream:
            mbr = stream.read(SECTOR_SIZE)
    except OSError as exc:
        return fail(f"cannot read image: {exc}")

    if size < SECTOR_SIZE or size % SECTOR_SIZE:
        return fail("image size must be a nonzero whole number of sectors")
    if len(mbr) != SECTOR_SIZE or mbr[510:512] != MBR_SIGNATURE:
        return fail("missing 0x55AA MBR signature")
    if args.partition_start_lba != EXPECTED_START_LBA:
        return fail("unexpected configured partition start LBA")
    if args.partition_sectors != EXPECTED_SECTORS:
        return fail("unexpected configured partition length")
    if args.kernel_end_lba <= 0:
        return fail("kernel end LBA must be positive")
    if args.kernel_end_lba > args.partition_start_lba:
        return fail("kernel payload overlaps the ProsenjitFS partition")

    entries = [mbr[446 + i * 16:446 + (i + 1) * 16] for i in range(4)]
    populated = [entry for entry in entries if entry[4] != 0 or
                 struct.unpack_from("<I", entry, 8)[0] != 0 or
                 struct.unpack_from("<I", entry, 12)[0] != 0]
    if len(populated) != 1:
        return fail(f"expected exactly one populated MBR entry, got {len(populated)}")

    entry = populated[0]
    start_lba, sectors = struct.unpack_from("<II", entry, 8)
    if entry[4] != EXPECTED_TYPE:
        return fail(f"partition type is 0x{entry[4]:02X}, expected 0xDA")
    if start_lba != args.partition_start_lba or sectors != args.partition_sectors:
        return fail(f"partition geometry is start={start_lba}, sectors={sectors}")
    if start_lba + sectors > size // SECTOR_SIZE:
        return fail("partition extends beyond the image")
    if start_lba + sectors == size // SECTOR_SIZE:
        return fail("image must reserve at least one trailing guard sector after the partition")

    print("native disk layout: OK")
    print(f"image_sectors={size // SECTOR_SIZE}")
    print(f"kernel_end_lba={args.kernel_end_lba}")
    print(f"partition_start_lba={start_lba}")
    print(f"partition_sectors={sectors}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
