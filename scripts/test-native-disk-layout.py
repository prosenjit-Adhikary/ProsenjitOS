#!/usr/bin/env python3
"""Regression tests for the read-only native BIOS disk-layout validator."""
from __future__ import annotations

import struct
import subprocess
import sys
import tempfile
from pathlib import Path

SECTOR_SIZE = 512
PARTITION_START = 2048
PARTITION_SECTORS = 512
IMAGE_SECTORS = PARTITION_START + PARTITION_SECTORS
ROOT = Path(__file__).resolve().parent.parent
VALIDATOR = ROOT / "scripts" / "verify-native-disk-layout.py"


def make_image(path: Path, *, signature: bool = True, part_type: int = 0xDA,
               start: int = PARTITION_START, sectors: int = PARTITION_SECTORS,
               extra_entry: bool = False, image_sectors: int = IMAGE_SECTORS) -> None:
    """Create a sparse synthetic image in a temporary directory only."""
    with path.open("wb") as stream:
        stream.truncate(image_sectors * SECTOR_SIZE)
    mbr = bytearray(SECTOR_SIZE)
    entry = 446
    mbr[entry + 4] = part_type
    struct.pack_into("<II", mbr, entry + 8, start, sectors)
    if extra_entry:
        mbr[462 + 4] = 0x83
        struct.pack_into("<II", mbr, 462 + 8, 10, 20)
    if signature:
        mbr[510:512] = b"\x55\xAA"
    with path.open("r+b") as stream:
        stream.write(mbr)


def run_validator(path: Path, *, kernel_end: int = PARTITION_START,
                  start: int = PARTITION_START,
                  sectors: int = PARTITION_SECTORS) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(VALIDATOR), str(path),
         "--kernel-end-lba", str(kernel_end),
         "--partition-start-lba", str(start),
         "--partition-sectors", str(sectors)],
        check=False, capture_output=True, text=True,
    )


def expect_rejected(name: str, **image_args: object) -> None:
    with tempfile.TemporaryDirectory(prefix="pros-native-layout-") as tmp:
        image = Path(tmp) / "fixture.img"
        kernel_end = int(image_args.pop("kernel_end", PARTITION_START))
        configured_start = int(image_args.pop("configured_start", PARTITION_START))
        configured_sectors = int(image_args.pop("configured_sectors", PARTITION_SECTORS))
        make_image(image, **image_args)
        result = run_validator(image, kernel_end=kernel_end,
                               start=configured_start, sectors=configured_sectors)
        if result.returncode == 0:
            raise AssertionError(f"{name}: invalid image unexpectedly accepted")
        if "native disk layout: ERROR:" not in result.stderr:
            raise AssertionError(f"{name}: missing diagnostic: {result.stderr!r}")


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="pros-native-layout-") as tmp:
        image = Path(tmp) / "valid.img"
        make_image(image)
        before = image.stat().st_size, image.read_bytes()
        result = run_validator(image)
        if result.returncode != 0 or "native disk layout: OK" not in result.stdout:
            raise AssertionError(f"valid fixture rejected: {result.stdout}{result.stderr}")
        after = image.stat().st_size, image.read_bytes()
        if before != after:
            raise AssertionError("validator modified its input image")

    expect_rejected("missing MBR signature", signature=False)
    expect_rejected("wrong partition type", part_type=0x83)
    expect_rejected("wrong partition start", start=2049)
    expect_rejected("wrong partition length", sectors=511)
    expect_rejected("extra populated partition", extra_entry=True)
    expect_rejected("partition exceeds image", image_sectors=IMAGE_SECTORS - 1)
    expect_rejected("kernel overlaps partition", kernel_end=PARTITION_START + 1)
    expect_rejected("wrong configured start", configured_start=2049)
    expect_rejected("wrong configured length", configured_sectors=511)

    with tempfile.TemporaryDirectory(prefix="pros-native-layout-") as tmp:
        image = Path(tmp) / "unaligned.img"
        image.write_bytes(b"x" * (SECTOR_SIZE + 1))
        result = run_validator(image)
        if result.returncode == 0:
            raise AssertionError("non-sector-aligned image unexpectedly accepted")

    print("native disk layout regression tests: PASS (1 valid + 10 invalid fixtures)")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"native disk layout regression tests: FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
