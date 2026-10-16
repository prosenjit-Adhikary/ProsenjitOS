#!/usr/bin/env python3
"""Regression tests for the read-only native BIOS image layout validator."""
from __future__ import annotations

import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VALIDATOR = ROOT / "scripts" / "verify-native-disk-layout.py"
SECTOR_SIZE = 512
IMAGE_SECTORS = 2560
PARTITION_START = 2048
PARTITION_SECTORS = 512


def make_image(path: Path, sectors: int = IMAGE_SECTORS) -> bytearray:
    image = bytearray(sectors * SECTOR_SIZE)
    image[446 + 4] = 0xDA
    struct.pack_into("<II", image, 446 + 8, PARTITION_START,
                     PARTITION_SECTORS)
    image[510:512] = b"\x55\xAA"
    path.write_bytes(image)
    return image


class NativeDiskLayoutValidatorTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tempdir = tempfile.TemporaryDirectory()
        self.image_path = Path(self.tempdir.name) / "native.img"

    def tearDown(self) -> None:
        self.tempdir.cleanup()

    def run_validator(self, *extra: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(VALIDATOR), str(self.image_path),
             "--kernel-end-lba", "100", *extra],
            text=True, capture_output=True, check=False,
        )

    def test_accepts_expected_layout_and_does_not_modify_image(self) -> None:
        original = bytes(make_image(self.image_path))
        result = self.run_validator()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("native disk layout: OK", result.stdout)
        self.assertEqual(self.image_path.read_bytes(), original)

    def test_rejects_missing_mbr_signature(self) -> None:
        image = make_image(self.image_path)
        image[510:512] = b"\x00\x00"
        self.image_path.write_bytes(image)
        result = self.run_validator()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("missing 0x55AA", result.stderr)

    def test_rejects_wrong_partition_type(self) -> None:
        image = make_image(self.image_path)
        image[446 + 4] = 0x83
        self.image_path.write_bytes(image)
        result = self.run_validator()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("partition type", result.stderr)

    def test_rejects_wrong_partition_geometry(self) -> None:
        image = make_image(self.image_path)
        struct.pack_into("<I", image, 446 + 12, PARTITION_SECTORS - 1)
        self.image_path.write_bytes(image)
        result = self.run_validator()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("partition geometry", result.stderr)

    def test_rejects_multiple_populated_entries(self) -> None:
        image = make_image(self.image_path)
        second = 446 + 16
        image[second + 4] = 0x83
        struct.pack_into("<II", image, second + 8, 100, 10)
        self.image_path.write_bytes(image)
        result = self.run_validator()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("exactly one populated", result.stderr)

    def test_rejects_partition_outside_image(self) -> None:
        make_image(self.image_path, sectors=2300)
        result = self.run_validator()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("extends beyond the image", result.stderr)

    def test_rejects_kernel_partition_overlap(self) -> None:
        make_image(self.image_path)
        result = self.run_validator("--kernel-end-lba", "2049")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("kernel payload overlaps", result.stderr)

    def test_rejects_non_sector_aligned_image(self) -> None:
        make_image(self.image_path)
        with self.image_path.open("ab") as stream:
            stream.write(b"x")
        result = self.run_validator()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("whole number of sectors", result.stderr)


if __name__ == "__main__":
    unittest.main()
