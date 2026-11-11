"""The rounding experiments tell the models apart, and pick the fake device's own (nearest)."""

import tempfile
import unittest
from pathlib import Path

from tools.benchloop import protocol, report, rop, session, texels


class RopTest(unittest.TestCase):
    def test_models_disagree_on_the_values_sent(self) -> None:
        vs = rop.values(8)
        for a in texels.ROUNDING:
            for b in texels.ROUNDING:
                if a != b:
                    self.assertTrue(any(texels.unorm(v, 8, a) != texels.unorm(v, 8, b) for v in vs), (a, b))

    def test_experiments_are_valid(self) -> None:
        for e in rop.generate():
            protocol.validate_experiment(e)

    def test_fake_device_rounds_to_nearest(self) -> None:
        planned = rop.generate(('bgra8unorm', 'rgb10a2unorm'))
        with tempfile.TemporaryDirectory() as scratch:
            results = session.fake(Path(scratch) / 'device').run_all(planned, 'rop', Path(scratch) / 'out')
        for row in report.analysis('rop', planned, results, [])['rop']:
            self.assertEqual(row['verdict'], ['nearest'], row['id'])


if __name__ == '__main__':
    unittest.main()
