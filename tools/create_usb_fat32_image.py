#!/usr/bin/env python3
"""Create a sparse, partitioned FAT32 image for the QEMU xHCI/MSC smoke test."""
import os
import struct
import sys

SECTOR = 512
IMAGE_BYTES = 128 * 1024 * 1024
IMAGE_SECTORS = IMAGE_BYTES // SECTOR
PARTITION_START = 2048
PARTITION_SECTORS = IMAGE_SECTORS - PARTITION_START
RESERVED_SECTORS = 32
FAT_COUNT = 2
SECTORS_PER_CLUSTER = 1


def make_fsinfo(cluster_count: int) -> bytes:
    sector = bytearray(SECTOR)
    struct.pack_into("<I", sector, 0, 0x41615252)
    struct.pack_into("<I", sector, 484, 0x61417272)
    struct.pack_into("<I", sector, 488, cluster_count - 1)
    struct.pack_into("<I", sector, 492, 3)
    struct.pack_into("<I", sector, 508, 0xAA550000)
    return bytes(sector)


def write_at(image, lba: int, data: bytes) -> None:
    if len(data) != SECTOR:
        raise ValueError("sector write must be exactly 512 bytes")
    image.seek(lba * SECTOR)
    image.write(data)


def create(path: str) -> None:
    fat_sectors = 1
    while True:
        data_sectors = (PARTITION_SECTORS - RESERVED_SECTORS -
                        FAT_COUNT * fat_sectors)
        clusters = data_sectors // SECTORS_PER_CLUSTER
        required = ((clusters + 2) * 4 + SECTOR - 1) // SECTOR
        if required == fat_sectors:
            break
        fat_sectors = required

    if clusters < 65525 or clusters + 2 > (fat_sectors * SECTOR // 4):
        raise RuntimeError("computed FAT32 geometry is invalid")
    data_start = RESERVED_SECTORS + FAT_COUNT * fat_sectors

    with open(path, "w+b") as image:
        image.truncate(IMAGE_BYTES)

        # MBR: one FAT32 LBA partition, starting at LBA 2048.
        mbr = bytearray(SECTOR)
        entry = 446
        mbr[entry] = 0x00
        mbr[entry + 1:entry + 4] = b"\xfe\xff\xff"
        mbr[entry + 4] = 0x0C
        mbr[entry + 5:entry + 8] = b"\xfe\xff\xff"
        struct.pack_into("<I", mbr, entry + 8, PARTITION_START)
        struct.pack_into("<I", mbr, entry + 12, PARTITION_SECTORS)
        mbr[510:512] = b"\x55\xaa"
        write_at(image, 0, bytes(mbr))

        # FAT32 BPB.
        boot = bytearray(SECTOR)
        boot[0:3] = b"\xeb\x58\x90"
        boot[3:11] = b"MSWIN4.1"
        struct.pack_into("<H", boot, 11, SECTOR)
        boot[13] = SECTORS_PER_CLUSTER
        struct.pack_into("<H", boot, 14, RESERVED_SECTORS)
        boot[16] = FAT_COUNT
        struct.pack_into("<H", boot, 17, 0)
        struct.pack_into("<H", boot, 19, 0)
        boot[21] = 0xF8
        struct.pack_into("<H", boot, 22, 0)
        struct.pack_into("<H", boot, 24, 63)
        struct.pack_into("<H", boot, 26, 255)
        struct.pack_into("<I", boot, 28, PARTITION_START)
        struct.pack_into("<I", boot, 32, PARTITION_SECTORS)
        struct.pack_into("<I", boot, 36, fat_sectors)
        struct.pack_into("<H", boot, 40, 0)
        struct.pack_into("<H", boot, 42, 0)
        struct.pack_into("<I", boot, 44, 2)
        struct.pack_into("<H", boot, 48, 1)
        struct.pack_into("<H", boot, 50, 6)
        boot[64] = 0x80
        boot[66] = 0x29
        struct.pack_into("<I", boot, 67, 0x50524F53)
        boot[71:82] = b"PROSUSB    "
        boot[82:90] = b"FAT32   "
        boot[510:512] = b"\x55\xaa"
        write_at(image, PARTITION_START, bytes(boot))
        write_at(image, PARTITION_START + 6, bytes(boot))

        fsinfo = make_fsinfo(clusters)
        write_at(image, PARTITION_START + 1, fsinfo)
        write_at(image, PARTITION_START + 7, fsinfo)

        # Two mirrored FATs: reserved entries and allocated root cluster 2.
        fat0 = bytearray(SECTOR)
        struct.pack_into("<III", fat0, 0, 0x0FFFFFF8,
                         0x0FFFFFFF, 0x0FFFFFFF)
        fat1_lba = PARTITION_START + RESERVED_SECTORS
        write_at(image, fat1_lba, bytes(fat0))
        write_at(image, fat1_lba + fat_sectors, bytes(fat0))

        # Root directory cluster 2 starts empty (the sparse image is zeroed).
        write_at(image, PARTITION_START + data_start, bytes(SECTOR))
        image.flush()
        os.fsync(image.fileno())

    print("FAT32 test image created:",
          f"size={IMAGE_BYTES}", f"partition_start={PARTITION_START}",
          f"partition_sectors={PARTITION_SECTORS}",
          f"fat_sectors={fat_sectors}", f"clusters={clusters}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} IMAGE")
    create(sys.argv[1])
