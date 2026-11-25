"""Tests for cpu.py's readers of the SwitchSched lines."""

import unittest
from datetime import UTC, datetime

from tools.perf import cpu
from tools.perf.source import Rec


def sched(msg: str, at: datetime) -> Rec:
    return Rec(
        at,
        0,
        proc='kernel',
        msg=msg,
        boot='B',
        pid=0,
        tid=1,
        subsystem='',
        category='',
        name='',
        kind='',
        ident=0,
    )


class CoreWaitsTest(unittest.TestCase):
    # 25 Nov: backboardd's main thread had no run for two windows, then one 32.6 s wait ending in the third.
    def test_a_wait_that_began_windows_earlier_keeps_its_start(self):
        recs = [
            sched(
                'SwitchSched 1/2 (10000 ms): backboardd.main[69] 53/53 TS IN FG: 10 runs 64/512/900 us',
                datetime(2026, 11, 25, 19, 16, 42, tzinfo=UTC),
            ),
            sched(
                'SwitchSched 1/2 (10000 ms): backboardd.main[69] 53/53 TS IN FG: 12 runs 64/32555950/32555950 us, '
                'longest began 22555 ms before the window; '
                'display.primary[53] 54/54 FX - FIX: 600 runs 32/64/90 us',
                datetime(2026, 11, 25, 19, 17, 12, tzinfo=UTC),
            ),
        ]
        waits = cpu.core_waits(recs)
        main = waits['backboardd.main']
        self.assertEqual((main.runs, main.worst_us, main.began_before_ms), (22, 32555950, 22555))
        self.assertEqual(main.when, datetime(2026, 11, 25, 19, 17, 12, tzinfo=UTC))
        self.assertEqual((waits['display.primary'].worst_us, waits['display.primary'].began_before_ms), (90, 0))


if __name__ == '__main__':
    unittest.main()
