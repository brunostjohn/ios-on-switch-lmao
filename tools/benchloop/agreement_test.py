"""The app checks a batch with the limits, formats and method ranges protocol.py checks it with.

hw_test_app/core/BenchProtocol.swift (the app's decoder) and hw_test_app/bench/bench_records.cpp (the GPU side's own
method check) are read as text, since neither runs on the host's Python.
"""

import re
import unittest
from pathlib import Path

from tools.benchloop import protocol

TREE = Path(__file__).resolve().parents[2]
SWIFT = TREE / 'hw_test_app' / 'core' / 'BenchProtocol.swift'
RECORDS = TREE / 'hw_test_app' / 'bench' / 'bench_records.cpp'
LIMITS = {
    'maxExperiments': 'MAX_EXPERIMENTS',
    'maxSide': 'MAX_SIDE',
    'maxDrawCounts': 'MAX_DRAW_COUNTS',
    'maxDraws': 'MAX_DRAWS',
    'maxRepeats': 'MAX_REPEATS',
    'maxWarmup': 'MAX_WARMUP',
    'maxCommandBuffers': 'MAX_COMMAND_BUFFERS',
    'maxCode': 'MAX_CODE',
    'maxData': 'MAX_DATA',
    'maxMethodPairs': 'MAX_METHOD_PAIRS',
    'maxGroups': 'MAX_GROUPS',
    'maxThreads': 'MAX_THREADS',
    'sphBytes': 'SPH_BYTES',
    'maxCommandBufferMs': 'MAX_COMMAND_BUFFER_MS',
    'clockHz': 'CLOCK_HZ',
    'lanesPerClock': 'LANES_PER_CLOCK',
}


def ranges(text: str, opening: str, closing: str) -> list[tuple[int, int]]:
    pattern = re.escape(opening) + r'(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+)' + re.escape(closing)
    return [(int(a, 16), int(b, 16)) for a, b in re.findall(pattern, text)]


class AgreementTest(unittest.TestCase):
    def test_limits(self) -> None:
        text = SWIFT.read_text()
        for swift, python in LIMITS.items():
            match = re.search(rf'static let {swift} = ([0-9.e* ]+)\n', text)
            self.assertIsNotNone(match, swift)
            self.assertEqual(eval(match.group(1)), getattr(protocol, python), swift)

    def test_formats(self) -> None:
        found = re.findall(r'"(\w+)": \((\d+), (\d+)\)', SWIFT.read_text())
        self.assertEqual({name: (int(raw), int(size)) for name, raw, size in found}, protocol.FORMATS)

    def test_method_ranges(self) -> None:
        self.assertEqual(ranges(SWIFT.read_text(), '(', ')'), list(protocol.METHOD_RANGES))
        self.assertEqual(ranges(RECORDS.read_text(), '{', '}'), list(protocol.METHOD_RANGES))

    def test_protocol_version(self) -> None:
        self.assertIn(f'static let version = "{protocol.PROTOCOL}"', SWIFT.read_text())


if __name__ == '__main__':
    unittest.main()
