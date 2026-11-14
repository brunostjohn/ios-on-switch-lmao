#!/usr/bin/env python3
"""The states the raw pack refuses for this IPSW kind are the pinned ones (//gpu/seed:raw_pack's report.txt).

Each refused render state and each kernel shape left out is one line, as the pack's link reports it (directlink.py:
"refused (<why>): <key, cut at 150>" and "skipped kernel <key>: <why>"). A refusal that appears fails until its form
exists or the pin names it; one that goes fails until the pin drops it. Empty: the pack refuses nothing.
usage: pack_refusals_test.py <pin> <the report output group's paths>...
"""

import sys
import unittest
from collections import Counter
from pathlib import Path

REFUSED = ('refused (', 'skipped kernel ')


def refusals(lines: list[str]) -> Counter:
    return Counter(line.strip() for line in lines if line.strip().startswith(REFUSED))


class PackRefusals(unittest.TestCase):
    def test_refusals_are_pinned(self) -> None:
        reports = [Path(p) / 'report.txt' for p in sys.argv[2:] if (Path(p) / 'report.txt').is_file()]
        self.assertEqual(len(reports), 1, sys.argv[2:])
        built = refusals(reports[0].read_text().splitlines())
        pinned = refusals(Path(sys.argv[1]).read_text().splitlines())
        self.assertEqual(sorted((built - pinned).elements()), [], 'refusals the pin does not name')
        self.assertEqual(sorted((pinned - built).elements()), [], 'pinned refusals the pack no longer has: unpin them')


if __name__ == '__main__':
    unittest.main(argv=sys.argv[:1])
