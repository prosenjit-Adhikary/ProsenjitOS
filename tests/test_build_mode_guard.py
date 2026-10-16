#!/usr/bin/env python3
"""Verify build-mode safety checks fail before creating or cleaning output directories."""
from __future__ import annotations

import os
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD_SCRIPT = ROOT / "scripts" / "build.sh"


class BuildModeGuardTests(unittest.TestCase):
    def run_invalid_mode(self, build_dir: str, persistence_mode: str,
                         expected_message: str) -> None:
        target = ROOT / build_dir
        self.assertFalse(target.exists(), f"test target unexpectedly exists: {target}")
        env = os.environ.copy()
        env["BUILD_DIR_NAME"] = build_dir
        env["NATIVE_PFS_PERSISTENCE_TEST"] = persistence_mode
        result = subprocess.run(
            ["bash", str(BUILD_SCRIPT)],
            cwd=ROOT,
            env=env,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn(expected_message, result.stderr)
        self.assertFalse(target.exists(), "invalid mode created/cleaned a build directory")

    def test_persistence_mode_requires_dedicated_output_directory(self) -> None:
        self.run_invalid_mode(
            "build-guard-test", "1",
            "NATIVE_PFS_PERSISTENCE_TEST=1 requires BUILD_DIR_NAME=build-persistence",
        )

    def test_dedicated_directory_requires_persistence_mode(self) -> None:
        self.run_invalid_mode(
            "build-persistence", "0",
            "build-persistence is reserved for NATIVE_PFS_PERSISTENCE_TEST=1",
        )

    def test_unknown_persistence_mode_is_rejected_early(self) -> None:
        self.run_invalid_mode(
            "build-guard-test", "maybe",
            "NATIVE_PFS_PERSISTENCE_TEST must be 0 or 1",
        )


if __name__ == "__main__":
    unittest.main()
