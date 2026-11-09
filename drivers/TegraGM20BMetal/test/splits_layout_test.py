#!/usr/bin/env python3
"""splits.py packs a split only where its per-pixel program binds its constant buffers as its whole program does.

A uniform buffer's constant buffer type holds its set-0 offset, and the twin binds the per-pixel program's buffers from
its state's set-0 entries: a type the whole program does not name would bind another buffer.
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'raw', 'pack'))
import directlink
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


class SplitIdentityTest(unittest.TestCase):
    def test_distinct_splits_share_whole_storage_and_keep_their_own_records(self):
        states = []
        for fill in (3, 4):
            split_record = packfile.PACK_SPLIT.pack(40, 0, 0, 0, 16, 3, 1, 0, 2, bytes([2]) + bytes(7), *([0] * 64))
            states.append(
                {
                    'programs': [program(1, [ROOT]), program(2, [ROOT, SET, ubo(48)])],
                    'split': {
                        'record': split_record,
                        'programs': [program(fill, [ROOT, SET, ubo(48), FRONTIER]), program(fill + 2, [4, ubo(48)])],
                    },
                }
            )
        states.append({'programs': [program(1, [ROOT]), program(2, [ROOT, SET, ubo(48)])]})
        directlink.split_identities(states)
        ids = [directlink.program_id(s['programs'][1]) for s in states]
        self.assertEqual(len(set(ids)), 3)
        programs = {directlink.program_id(p): p for s in states for p in s['programs']}
        found = directlink.split_programs(states, programs)
        laid = (bytearray(), {}, [])
        for pid in ids:
            packfile.place_graphics(pid, programs, *laid)
        whole_bytes = len(laid[0])
        self.assertEqual(whole_bytes, 256)
        self.assertEqual({p['heap_offset'] for p in laid[2]}, {0})
        packed = splits.lay_out(ids, found, programs, laid, [])
        self.assertEqual([s['whole'] for s in packed], [0, 1])
        for state, split in zip(states[:2], packed, strict=True):
            self.assertEqual(laid[2][split['fs']]['id'], directlink.program_id(state['split']['programs'][0]))
            self.assertEqual(laid[2][split['prologue']]['id'], directlink.program_id(state['split']['programs'][1]))
        self.assertNotIn(ids[2], found)


if __name__ == '__main__':
    unittest.main()
