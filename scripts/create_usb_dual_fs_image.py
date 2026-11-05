#!/usr/bin/env python3
"""Build a QEMU USB MSC disk containing one FAT32 and one exFAT MBR partition."""
import os
import struct
import subprocess
import sys
import tempfile

SECTOR = 512
IMAGE_BYTES = 256 * 1024 * 1024
FAT_START = 2048
EXFAT_START = 131072
EXFAT_SECTORS = (IMAGE_BYTES // SECTOR) - EXFAT_START
FAT_SECTORS = EXFAT_START - FAT_START


def partition_entry(mbr: bytearray, slot: int, kind: int,
                    start: int, sectors: int) -> None:
    offset = 446 + slot * 16
    mbr[offset] = 0
    mbr[offset + 1:offset + 4] = b"\xfe\xff\xff"
    mbr[offset + 4] = kind
    mbr[offset + 5:offset + 8] = b"\xfe\xff\xff"
    struct.pack_into("<I", mbr, offset + 8, start)
    struct.pack_into("<I", mbr, offset + 12, sectors)


def create(path: str) -> None:
    if os.path.exists(path):
        os.remove(path)
    with open(path, "w+b") as disk:
        disk.truncate(IMAGE_BYTES)
        mbr = bytearray(SECTOR)
        partition_entry(mbr, 0, 0x0C, FAT_START, FAT_SECTORS)
        partition_entry(mbr, 1, 0x07, EXFAT_START, EXFAT_SECTORS)
        mbr[510:512] = b"\x55\xaa"
        disk.seek(0)
        disk.write(mbr)
        disk.flush()
        os.fsync(disk.fileno())

    with tempfile.NamedTemporaryFile(prefix="prosenjitos-fat32-",
                                     suffix=".img", delete=False) as tmp:
        fat_path = tmp.name
        tmp.truncate(FAT_SECTORS * SECTOR)
    with tempfile.NamedTemporaryFile(prefix="prosenjitos-exfat-",
                                     suffix=".img", delete=False) as tmp:
        exfat_path = tmp.name
        tmp.truncate(EXFAT_SECTORS * SECTOR)
    try:
        subprocess.run(["mformat", "-F", "-v", "PROSFAT", "-i", fat_path, "::"], check=True, timeout=30)
        subprocess.run(["mkfs.exfat", "-L", "PROSEX", exfat_path],
                       check=True, timeout=30)
        with open(path, "r+b") as disk:
            for offset, source_path, byte_count in (
                (FAT_START * SECTOR, fat_path, FAT_SECTORS * SECTOR),
                (EXFAT_START * SECTOR, exfat_path, EXFAT_SECTORS * SECTOR),
            ):
                disk.seek(offset)
                with open(source_path, "rb") as source:
                    remaining = byte_count
                    while remaining:
                        chunk = source.read(min(1024 * 1024, remaining))
                        if not chunk:
                            raise RuntimeError("short read from formatted filesystem image")
                        disk.write(chunk)
                        remaining -= len(chunk)
            disk.flush()
            os.fsync(disk.fileno())
    finally:
        os.unlink(fat_path)
        os.unlink(exfat_path)

    print("Dual-filesystem USB image created:",
          f"FAT32={FAT_START}+{FAT_SECTORS}",
          f"exFAT={EXFAT_START}+{EXFAT_SECTORS}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} IMAGE")
    create(sys.argv[1])
