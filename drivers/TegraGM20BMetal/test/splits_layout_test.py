#!/usr/bin/env python3
"""splits.py lays out each split's per-pixel program with its whole program's constant buffer types.

Two wholes whose per-pixel parts compile to the same bytes share one programs.bin record, whose types are the first
compile's. A uniform buffer's type holds its set-0 offset: a twin bound by another whole's types reads another buffer.
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'raw', 'pack'))
import packfile
import splits

ROOT, SET, FRONTIER = 1, 3, 6


def ubo(offset: int) -> int:
    return 5 | (offset // 16) << 3


def program(fill: int, cbufs: list[int]) -> dict:
    return {'image': bytes([fill]) * 96, 'cbufs': cbufs, 'data': b''}


def split(fs: str) -> dict:
    return {
        'fs': fs,
        'prologue': 'p',
        'frontier_cbuf': 3,
        'prologue_cbufs': 2,
        'ubo_cbufs': [2],
        'stats': (10, 0, 8, 0, 3, 0),
    }


class PerPixelTest(unittest.TestCase):
    def programs(self) -> dict[str, dict]:
        return {
            'a': program(1, [ROOT, SET, ubo(48)]),
            'b': program(2, [ROOT, SET, ubo(32)]),
            'f': program(3, [ROOT, SET, ubo(48), FRONTIER]),
            'p': {'image': bytes(80) + bytes(16), 'cbufs': [4, ubo(48)], 'data': b''},
        }

    def test_each_whole_binds_its_own_offsets(self) -> None:
        programs = self.programs()
        laid = (bytearray(), {}, [])
        for whole in ('a', 'b'):
            packfile.place_graphics(whole, programs, *laid)
        out = splits.lay_out(['a', 'b'], {'a': split('f'), 'b': split('f')}, programs, laid, [])
        _, _, recs = laid
        by_whole = {recs[s['whole']]['id']: recs[s['fs']] for s in out}
        self.assertEqual(by_whole['a']['cbufs'], [ROOT, SET, ubo(48), FRONTIER])
        self.assertEqual(by_whole['b']['cbufs'], [ROOT, SET, ubo(32), FRONTIER])
        self.assertEqual(by_whole['b']['id'], 'f')

    def test_a_frontier_past_the_wholes_buffers_is_left_out(self) -> None:
        programs = self.programs()
        self.assertIsNone(splits.per_pixel(dict(split('f'), frontier_cbuf=2), 'a', programs))


if __name__ == '__main__':
    unittest.main()
