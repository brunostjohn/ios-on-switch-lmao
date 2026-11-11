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
        self.assertEqual(
            {e.get('compare') for e in experiments} - {None}, {'program', 'copy', 'resolve3d', 'sampled3d'}
        )
        last = experiments[-1]['twod']
        self.assertEqual(last['colorFormat'], twod.F32, 'the float form that hung runs last')
        self.assertTrue(last['clearFirst'] and not last['bracketed'], 'after a clear-only pass, with no waits')
        self.assertTrue(last['tiled'], "on the probe's layout: a private texture is block-linear")
        scales = [e for e in experiments if e['twod']['op'] in ('scale', 'resolve') and 'source' in e]
        self.assertTrue(all(e['source']['format'] == e['target']['format'] for e in scales))

    def test_no_two_by_two_mean_of_the_source_is_a_tie(self) -> None:
        data, side = twod.source_texels(), 64
        for y in range(0, side, 2):
            for x in range(0, side, 2):
                for c in range(3):
                    block = [data[((y + dy) * side + x + dx) * 4 + c] for dy in (0, 1) for dx in (0, 1)]
                    self.assertNotEqual(sum(block) % 4, 2, (x, y, c))

    def test_clipped_and_cleared_cells_are_judged_by_what_they_write(self) -> None:
        cells = {e['id']: e for e in twod.generate()}
        clip = cells['fill-own-clip-bgra8unorm']
        self.assertEqual(twod.written_box(clip), (4, 4, 12, 10))
        side, want = twod.SIDE, twod.wanted_fill(clip)
        data = bytearray(bytes([twod.FILL_BYTE]) * (side * side * 4))
        for y in range(4, 10):
            for x in range(4, 12):
                data[(y * side + x) * 4 : (y * side + x + 1) * 4] = want
        result = {'status': 'ok', 'texels': {'data': protocol.b64(bytes(data))}}
        row = twod.analyse([clip], {clip['id']: result})[0]
        self.assertTrue(row['kept_outside'])
        self.assertEqual(row['fill_exact'], row['fill_cells'])
        self.assertEqual(twod.background(cells['fill-own-afterclear-bgra8unorm']), bytes(4))

    def test_a_resolve_is_judged_against_the_box_mean_of_its_samples(self) -> None:
        cell = next(e for e in twod.generate() if e['id'] == 'resolve-sweep-corner-bil-p00')
        self.assertEqual((cell['twod']['sx'], cell['twod']['du']), (1 << 32, 2 << 32))
        src, side = protocol.unb64(cell['source']['data']), 64
        box = bytes(
            (sum(src[((2 * y + j) * side + 2 * x + i) * 4 + c] for j in (0, 1) for i in (0, 1)) + 2) >> 2
            for y in range(32)
            for x in range(32)
            for c in range(4)
        )
        self.assertEqual(twod.box_mismatches(cell, box)['nearest_up'], 0)
        self.assertGreater(twod.box_mismatches(cell, box)['truncate'], 0, 'the source has sums that round')
        point = next(e for e in twod.generate() if e['id'] == 'resolve-sweep-corner-point-p00')
        sample = bytes(
            src[((2 * y + 1) * side + 2 * x) * 4 + c] for y in range(32) for x in range(32) for c in range(4)
        )
        self.assertEqual(twod.point_sample(point, sample), (0, 1))
        self.assertIsNone(twod.point_sample(point, box))
        sweep = [e for e in twod.generate() if e['id'].startswith('resolve-sweep-')]
        self.assertEqual(len(sweep), 2 * (17 + 17))

    def test_fills_run_and_are_judged_on_the_fake_device(self) -> None:
        planned = [e for e in twod.generate() if e['id'] in ('fill-own-bgra8unorm', 'fill-own-ref-rgba8unorm')]
        with tempfile.TemporaryDirectory() as scratch:
            results = session.fake(Path(scratch) / 'device').run_all(planned, 'twod', Path(scratch) / 'out')
        rows = report.analysis('twod', planned, results, [])['twod']
        self.assertEqual([r['status'] for r in rows], ['ok', 'ok'])
        self.assertTrue(all(r['kept_outside'] and r['fill_exact'] == r['fill_cells'] == 14 * 14 for r in rows), rows)


if __name__ == '__main__':
    unittest.main()
