#!/usr/bin/env python3
"""Unit tests for prune_disk_cache.py."""

import os
import tempfile
import time
import unittest
from pathlib import Path

from tools.ci import prune_disk_cache

SEC_NS = 1_000_000_000


def _write_sized(path: Path, size: int, mtime_ns: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b'x' * size)
    os.utime(path, ns=(mtime_ns, mtime_ns))


class PruneDiskCacheTest(unittest.TestCase):
    def test_skips_save_when_nothing_newer_than_marker(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            disk = Path(td) / 'disk'
            now = time.time_ns()
            _write_sized(disk / 'cas/00/old', 100, now - 60 * SEC_NS)
            result = prune_disk_cache.prune_disk_cache(disk, now, max_bytes=1000, target_bytes=800, max_age_days=14)
            self.assertFalse(result['save'])
            self.assertTrue((disk / 'cas/00/old').exists())

    def test_keeps_untouched_warm_entries_below_cap(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            disk = Path(td) / 'disk'
            now = time.time_ns()
            _write_sized(disk / 'cas/00/from_prev_job', 200, now - 3600 * SEC_NS)
            _write_sized(disk / 'cas/01/from_this_run', 200, now + 5 * SEC_NS)
            result = prune_disk_cache.prune_disk_cache(disk, now, max_bytes=1000, target_bytes=800, max_age_days=14)
            self.assertTrue(result['save'])
            self.assertEqual(result['evicted_files'], 0)
            self.assertTrue((disk / 'cas/00/from_prev_job').exists())
            self.assertTrue((disk / 'cas/01/from_this_run').exists())

    def test_evicts_oldest_untouched_and_consistent_ac_above_cap(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            disk = Path(td) / 'disk'
            now = time.time_ns()
            _write_sized(disk / 'ac/00/oldest_ac', 50, now - 500 * SEC_NS)
            _write_sized(disk / 'cas/00/oldest_cas', 400, now - 400 * SEC_NS)
            _write_sized(disk / 'cas/01/recent_prev', 300, now - 100 * SEC_NS)
            _write_sized(disk / 'cas/02/current_run', 300, now + 5 * SEC_NS)
            result = prune_disk_cache.prune_disk_cache(disk, now, max_bytes=900, target_bytes=650, max_age_days=14)
            self.assertTrue(result['save'])
            self.assertFalse((disk / 'cas/00/oldest_cas').exists())
            self.assertFalse((disk / 'ac/00/oldest_ac').exists())
            self.assertTrue((disk / 'cas/01/recent_prev').exists())
            self.assertTrue((disk / 'cas/02/current_run').exists())

    def test_evicts_expired_entries_beyond_max_age(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            disk = Path(td) / 'disk'
            now = time.time_ns()
            _write_sized(disk / 'cas/00/ancient', 100, now - 20 * 86400 * SEC_NS)
            _write_sized(disk / 'cas/01/fresh', 100, now + 5 * SEC_NS)
            result = prune_disk_cache.prune_disk_cache(disk, now, max_bytes=10000, target_bytes=8000, max_age_days=14)
            self.assertTrue(result['save'])
            self.assertEqual(result['evicted_files'], 1)
            self.assertFalse((disk / 'cas/00/ancient').exists())
            self.assertTrue((disk / 'cas/01/fresh').exists())


if __name__ == '__main__':
    unittest.main()
