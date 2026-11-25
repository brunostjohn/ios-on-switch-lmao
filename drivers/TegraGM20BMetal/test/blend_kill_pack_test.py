#!/usr/bin/env python3
"""The built pack's one-target source-over states take the ROP's zero-source kill (blend_kill.h, pack/segments.py).

HW Test gpu.gm20b.blend_kill counts no state made with it (kill_states=0) on every boot. A record's blob holds only its
own targets' SET_CT_WRITE (mkpack.foreign); another target's comes from the baseline, which keeps a target's register
there when every state that draws that target writes it alike. So a one-target state reads targets 1-7 as written.
They are no attachments of its passes (a pass's targets are its state's key), so the kill must not count them.

usage: blend_kill_pack_test.py <pack.bin>   (//gpu/seed:pack's, of the configured IPSW kind)
"""

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE, os.path.join(HERE, '..', 'pack')]
import make_pack_targets_test as targets
import segments

SET_CT_WRITE, SET_BLEND, PER_TARGET = 0x1A00, 0x1360, 0x1E04
SOURCE_OVER = (0x8006, 0x4001, 0x4303)  # ADD, ONE, ONE_MINUS_SRC_ALPHA
PACK = ''


def source_over_on_target_0_alone(record: targets.Record) -> bool:
    """Whether a record draws target 0 alone, blending source-over on colour and alpha"""
    regs = record.regs
    blend = tuple(regs.get(PER_TARGET + 4 * i) for i in range(6))
    return targets.drawn_targets(record) == 1 and regs.get(SET_BLEND) == 1 and blend == SOURCE_OVER * 2


class BlendKillPackTest(unittest.TestCase):
    def test_one_target_source_over_states_take_the_kill(self) -> None:
        records, baseline = targets.load(PACK)
        base_writes = [baseline.get(SET_CT_WRITE + 4 * j, 0) for j in range(8)]
        base_blend = [baseline.get(PER_TARGET + 4 * i, 0) for i in range(6)]
        base_seen = sum(1 << i for i in range(6) if PER_TARGET + 4 * i in baseline)
        over = [r for r in records if source_over_on_target_0_alone(r)]
        killed = []
        for r in over:
            writes = list(base_writes)
            for j in range(8):
                writes[j] = r.regs.get(SET_CT_WRITE + 4 * j, writes[j])
            key = r.key.split(' | ')[0].rstrip()
            if segments._iterated_blend_noop(list(r.regs.items()), writes, key, 0, base_blend, base_seen):
                killed.append(r)
        self.assertGreater(len(over), 0, 'no one-target source-over record in the pack')
        self.assertGreater(
            len(killed),
            0,
            f'{len(over)} one-target source-over records, none killed; the baseline SET_CT_WRITE of targets 0-7: '
            f'{[f"{w:#x}" for w in base_writes]}',
        )


if __name__ == '__main__':
    PACK = sys.argv.pop(1)
    unittest.main()
