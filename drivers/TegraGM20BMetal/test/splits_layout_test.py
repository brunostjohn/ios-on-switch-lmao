#!/usr/bin/env python3
"""splits.py packs a split only where its per-pixel program binds its constant buffers as its whole program does.

A uniform buffer's constant buffer type holds its set-0 offset, and the twin binds the per-pixel program's buffers from
its state's set-0 entries: a type the whole program does not name would bind another buffer.
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


def split(fs: str, frontier_cbuf: int = 3) -> dict:
    return {'fs': fs, 'prologue': 'p', 'frontier_cbuf': frontier_cbuf, 'prologue_cbufs': 2, 'ubo_cbufs': [2]}


class LayOutTest(unittest.TestCase):
    def programs(self) -> dict[str, dict]:
        return {
            'a': program(1, [ROOT, SET, ubo(48)]),
            'b': program(2, [ROOT, SET, ubo(32)]),
            'fa': program(3, [ROOT, SET, ubo(48), FRONTIER]),
            'p': {'image': bytes(80) + bytes(16), 'cbufs': [4, ubo(48)], 'data': b''},
        }

    def lay_out(self, found: dict[str, dict]) -> list[str]:
        programs = self.programs()
        laid = (bytearray(), {}, [])
        for whole in ('a', 'b'):
            packfile.place_graphics(whole, programs, *laid)
        out = splits.lay_out(['a', 'b'], found, programs, laid, [])
        return [laid[2][s['whole']]['id'] for s in out]

    def test_a_split_bound_as_its_whole_is_packed(self) -> None:
        self.assertEqual(self.lay_out({'a': split('fa')}), ['a'])

    def test_a_split_naming_another_offset_is_left_out(self) -> None:
        self.assertEqual(self.lay_out({'b': split('fa')}), [])

    def test_a_frontier_not_after_the_wholes_buffers_is_left_out(self) -> None:
        self.assertEqual(self.lay_out({'a': split('fa', frontier_cbuf=2)}), [])


if __name__ == '__main__':
    unittest.main()
