"""tools/perf's reading of a flight recorder dump, the ring and the stackshot beside it.

The ring is RAW kdebug as kdbg_dump_trace_to_file writes it; the stackshot's names and images are as XNU's kcdata.py
prints them.
"""

import json
import os
import tempfile
import unittest
from datetime import UTC, datetime, timedelta

from tools.perf import cpu, dumps, ktrace, session, windows
from tools.perf.source import TICK_HZ, Rec

MAIN, RENDER, SUBMITTER = 200, 100, 300
DSC_BASE, DSC_SLIDE = 0x190000000, 0x10000000
APP_BASE = 0x100000000
APP_UUID = '0123456789abcdef0123456789abcdef'


def tick(ms: float) -> int:
    return 10_000_000_000 + int(ms * TICK_HZ / 1000)


def event(ms: float, debugid: int, args: tuple, tid: int, cpu_id: int = 0) -> bytes:
    a = (*args, 0, 0, 0, 0)[:4]
    return ktrace.EVENT.pack(tick(ms), *a, tid, debugid, cpu_id, 0)


def ring() -> bytes:
    """Return a ring as the kernel writes it with no thread map: 384 padding entries, events from 16 KiB."""
    head = ktrace.HEADER.pack(ktrace.RAW_VERSION1, 384, 1_795_400_000, 0)
    events = [
        event(0, ktrace.MACH_SCHED, (0, MAIN, 31, 47), RENDER),  # cpu0: the render thread switches to the main thread
        event(100, ktrace.code(37, 2, 6), (0, 3), MAIN),  # kperf: a user stack of three frames
        event(100.01, ktrace.code(37, 2, 4), (DSC_BASE + 0x40, APP_BASE + 0x10, DSC_BASE + 0x80, 0), MAIN),
        event(150, ktrace.SUBMIT, (7, 5, 1), SUBMITTER),
        event(260, ktrace.GPU_JOB, ((5 << 32) | (1 << 16) | 7, 1_000_000, 4_000_000), 0),
        event(300, ktrace.MACH_SCHED, (0, RENDER, 47, 31), MAIN),  # the main thread leaves the core after 300 ms
        event(400, ktrace.TRIGGER, (2, MAIN, 412), 0),
        event(401, ktrace.DUMP | ktrace.FUNC_START, (2, 0), 0),
    ]
    body = b''.join(events)
    return head + bytes(16384 - len(head)) + body


def kcdata() -> dict:
    return {
        'kcdata_stackshot': {
            'task_snapshots': {
                '1': {
                    'task_snapshot': {'ts_pid': 42, 'ts_p_comm': 'SwitchHWTest'},
                    'thread_snapshots': {'200': {'thread_snapshot': {'ths_thread_id': MAIN}, 'pth_name': 'main'}},
                    'dyld_load_info': [{'imageLoadAddress': APP_BASE, 'imageUUID': list(bytes.fromhex(APP_UUID))}],
                    'shared_cache_dyld_load_info': {'imageSlidBaseAddress': DSC_BASE, 'imageLoadAddress': DSC_SLIDE},
                },
            }
        }
    }


class DumpTest(unittest.TestCase):
    def test_a_hang_dump_names_its_thread_and_its_stacks(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, 'Switch-recorder-0')
            with open(path + '.ktrace', 'wb') as f:
                f.write(ring())
            with open(path + '.kcdata', 'wb') as f:
                f.write(b'kcdata')
            decoder = os.path.join(d, 'kcdata.py')
            with open(decoder, 'w') as f:
                f.write(f'import json\nprint(json.dumps({json.dumps(kcdata())}))\n')
            self.assertEqual(dumps.find(d), [(path + '.ktrace', path + '.kcdata')])
            uuid = '01234567-89AB-CDEF-0123-456789ABCDEF'
            text = '\n'.join(dumps.summary(path + '.ktrace', path + '.kcdata', decoder, {uuid: '/x/SwitchHWTest'}))
        self.assertIn('Switch-recorder-0.ktrace: hang, thread SwitchHWTest[42] main (200), value 412', text)
        self.assertIn('its thread: on a core 300 ms of the last 1000', text)
        self.assertIn('leaves: dsc 0x180000040 1', text)
        self.assertIn('most common stack (1): dsc 0x180000040 < SwitchHWTest+0x10 < dsc 0x180000080', text)
        self.assertIn('GPU: 1 jobs ended', text)
        self.assertIn('longest job channel 7 seqno 5 (compositor) 3.0 ms', text)

    def test_a_ring_that_is_not_raw_kdebug_is_refused(self):
        with self.assertRaises(ValueError):
            ktrace.parse(bytes(64))


class MemoryClockTest(unittest.TestCase):
    def test_a_window_names_the_memory_clocks_residency(self):
        t0 = datetime(2026, 11, 23, 0, 15, 26, tzinfo=UTC)
        line = (
            'SwitchPower: 10005 ms: cpu0 c7 0.0% (0, 0 skipped, 0 frame wakes) wfi 32.2% busy 67.8%; EMC 3 moves '
            '204:46.7% 1331:0.9% 1600:52.4%, 2 raises'
        )
        rec = Rec(t0 + timedelta(seconds=10), 0, 'B', 'kernel', 0, 0, '', '', '', '', 0, line)
        s = session.build('B', [rec])
        w = windows.Window('launch', 'x', t0 + timedelta(seconds=2), t0 + timedelta(seconds=9))
        self.assertEqual(
            {k: round(v, 3) for k, v in cpu.mean_emc(w, s.busy).items()}, {204: 0.467, 1331: 0.009, 1600: 0.524}
        )
        self.assertIn('memory clock: 1600 MHz 52%, 204 MHz 47%, 1331 MHz 1%', '\n'.join(session.dossier(s, w)))


if __name__ == '__main__':
    unittest.main()
