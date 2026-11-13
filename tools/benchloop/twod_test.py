"""The 2D batch is valid, its source has no equal-weight ties, and its fills run and are judged on the fake device."""

import tempfile
import unittest
from pathlib import Path

from tools.benchloop import protocol, report, session, twod


class TwodTest(unittest.TestCase):
    def test_every_operation_is_valid_and_named_once(self) -> None:
        experiments = twod.generate()
        self.assertEqual(len({e['id'] for e in experiments}), len(experiments))
        protocol.batch('twod', experiments)
        ops = {e['twod']['op'] for e in experiments}
        self.assertEqual(ops, {'fill', 'scale', 'resolve'})
        self.assertEqual({e.get('compare') for e in experiments} - {None}, {'program', 'copy', 'resolve3d'})
        self.assertEqual(experiments[-1]['twod']['colorFormat'], twod.F32, 'the float form that hung runs last')

    def test_no_two_by_two_mean_of_the_source_is_a_tie(self) -> None:
        data, side = twod.source_texels(), 64
        for y in range(0, side, 2):
            for x in range(0, side, 2):
                for c in range(3):
                    block = [data[((y + dy) * side + x + dx) * 4 + c] for dy in (0, 1) for dx in (0, 1)]
                    self.assertNotEqual(sum(block) % 4, 2, (x, y, c))

    def test_fills_run_and_are_judged_on_the_fake_device(self) -> None:
        planned = [e for e in twod.generate() if e['id'] in ('fill-own-bgra8unorm', 'fill-own-ref-rgba8unorm')]
        with tempfile.TemporaryDirectory() as scratch:
            results = session.fake(Path(scratch) / 'device').run_all(planned, 'twod', Path(scratch) / 'out')
        rows = report.analysis('twod', planned, results, [])['twod']
        self.assertEqual([r['status'] for r in rows], ['ok', 'ok'])
        self.assertTrue(all(r['kept_outside'] and r['fill_exact'] == r['fill_cells'] == 14 * 14 for r in rows), rows)


if __name__ == '__main__':
    unittest.main()
