# SPDX-License-Identifier: MIT
"""Proves byte-for-byte SM53 compiler parity between Rust 1.96.1 (:host_cc) and Rust 1.98.0 (:desktop)."""

import hashlib
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


class SwitchCompileDesktopParityTest(unittest.TestCase):
    def test_sm53_byte_parity(self) -> None:
        host_bin = sys.argv[1]
        desktop_bin = sys.argv[2]
        shaders = sys.argv[3:]
        self.assertEqual(len(shaders), 6)

        with tempfile.TemporaryDirectory() as tmp:
            host_out = Path(tmp) / 'host_1_96_1.bin'
            desktop_out = Path(tmp) / 'desktop_1_98_0.bin'

            host_res = subprocess.run(
                [host_bin, str(host_out), *shaders],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(
                host_res.returncode,
                0,
                f'host (1.96.1) failed:\nstdout:\n{host_res.stdout}\nstderr:\n{host_res.stderr}',
            )

            desktop_res = subprocess.run(
                [desktop_bin, str(desktop_out), *shaders],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(
                desktop_res.returncode,
                0,
                f'desktop (1.98.0) failed:\nstdout:\n{desktop_res.stdout}\nstderr:\n{desktop_res.stderr}',
            )

            host_bytes = host_out.read_bytes()
            desktop_bytes = desktop_out.read_bytes()
            host_sha = hashlib.sha256(host_bytes).hexdigest()
            desktop_sha = hashlib.sha256(desktop_bytes).hexdigest()

            print('--- Rust 1.96.1 (:host_cc) ---')
            print(host_res.stdout.strip())
            print(f'total_bytes={len(host_bytes)} sha256={host_sha}')
            print('--- Rust 1.98.0 (:desktop) ---')
            print(desktop_res.stdout.strip())
            print(f'total_bytes={len(desktop_bytes)} sha256={desktop_sha}')

            self.assertEqual(host_res.stdout, desktop_res.stdout)
            if host_bytes != desktop_bytes:
                first_diff = next(
                    (i for i, (a, b) in enumerate(zip(host_bytes, desktop_bytes)) if a != b),
                    min(len(host_bytes), len(desktop_bytes)),
                )
                self.fail(
                    f'Byte mismatch at offset {first_diff} '
                    f'(host len={len(host_bytes)} sha256={host_sha}, '
                    f'desktop len={len(desktop_bytes)} sha256={desktop_sha})'
                )


if __name__ == '__main__':
    unittest.main(argv=sys.argv[:1])
