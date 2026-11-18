"""tools/perf over lines as boot 2 of 22 Nov logged them (iOS 27, a debug kernel), cut to what each test needs."""

import json
import os
import tempfile
import unittest
from datetime import UTC, datetime, timedelta

from tools.perf import ips, pacing, perf, report, session, source, windows, work
from tools.perf.source import TICK_HZ, Rec

T0 = datetime(2026, 11, 23, 0, 13, 20, tzinfo=UTC)
MACH0 = 25_640_000_000
PERIOD_TICKS = int(TICK_HZ / 60)

# A present two periods after the one before, its first submission 22.5 ms after the vsync it started from.
LATE_RECORD = (
    'SwitchFrame: latch {latch} after 2 frames (33331 us), cause ca; origin vsync {origin}; first submit 22523, '
    'last submit 22523 (1, exec 20 us), swap 22763, offer 22895, gpu done 24133, flip 24047, latch 33331 us; other '
    'submits 0; swap thread base 54 sched 54 mode 2 qos 0; cpu 1785 MHz at vsync 1785 at swap; imp 0 0 us, bind 0 '
    '0 us, alloc 0 0 us, free 0, clean 0 KiB 0 us'
)
# One whose own chain latched a period after its origin: the frame before it never presented.
GAP = (
    'SwitchFrame: latch {latch} after 2 frames (33333 us), cause ca; origin vsync {origin}; first submit 8700, '
    'last submit 8700 (1, exec 20 us), swap 9000, offer 9100, gpu done 10900, flip 10900, latch 16700 us; other '
    'submits 0; swap thread base 54 sched 54 mode 2 qos 0; cpu 1785 MHz at vsync 1785 at swap'
)
QUEUED = (
    'SwitchFrame: latch {latch} after 13 frames (216700 us), cause queue; origin vsync {origin}; first submit '
    '2700, last submit 2700 (1, exec 20 us), swap 2800, offer 215900, gpu done -1, flip 215900, latch 216700 us; '
    'other submits 0; swap thread base 54 sched 54 mode 2 qos 0; cpu 1785 MHz at vsync 1785 at swap'
)
FROM_SWAP = (
    'SwitchFrame: latch {latch} after 3 frames (50005 us), cause ca; origin swap {origin}; first submit -204, last '
    'submit -204 (1, exec 16 us), swap 0, offer 106, gpu done -1, flip 127, latch 12633 us; other submits 0; swap '
    'thread base 54 sched 54 mode 2 qos 0; cpu 1785 MHz at vsync 1785 at swap'
)
# The render thread's line for a vsync: its swap, how it spent the time to it, and its longest block.
RENDER = (
    'SwitchFrameRender: vsync {vsync}: woke 61 us after it; first submit none; swap {swap} us (cpu {cpu}, blocked '
    '{blocked} us in 3, waiting for a core 34 us); longest block {longest} us, user pc dsc 0x23c991cd4 lr dsc '
    '0x23c995308, kernel continuation 0x0; dropped 0'
)
USER = (
    'SwitchFrameUser: backboardd cb 27305 commit {commit} enc {enc} {enc2} replay 0 0 ret 0 owait 0 0 fence {done} '
    'done {done} back 0 slot 0 0 prev 0 dropped 0 surf 1 imp 1 hit 0 made 0'
)
STATS = (
    'SwitchDisplay stats: 5016 ms: 262 DC A interrupts, 263 swaps 52.4/s; present 262 (262 timed): interval p50 17 '
    'p99 50 p99.9 50 max 50.0 ms, frames 1:239 2:7 3:16 4+:0, >1.5x 23 >2x 16, gaps 0/0; swap-to-scan p50 9'
)
POWER = (
    'SwitchPower: 10005 ms: cpu0 c7 0.0% (0, 0 skipped, 0 frame wakes) wfi 32.2% busy 67.8%, cpu1 c7 0.0% (0, 0 '
    'skipped, 0 frame wakes) wfi 37.2% busy 62.8%; c7 timer exits 0'
)
# probes-kernel's per-window CPU lines, as tools/devicerun/cpu_test.py writes them
CORES = (
    "SwitchSched cores (10000 ms; ms: a core's threads, its idle thread split; 310 us to take): cpu0 threads 4000 "
    '(irq 300), idle 6000 = asleep 5700 + irq 150 + rest 150, 12000 sleeps; cpu1 threads 1000 (irq 10), idle 9000 = '
    'asleep 8800 + irq 100 + rest 100, 8000 sleeps; a64x traps 0, unhandled 0, text pages 12'
)
TASKS = (
    'SwitchSched tasks (10000 ms; ms user+system, top 8 by both): SpringBoard[393] 1200+300, backboardd[69] 500+200; '
    'all 312 processes 3900+1800, exited 2 (410+30, most prngseedd[1234] 80+2), untracked 0'
)
MADE = (
    'SwitchMetal: a state the pack lacks made in 287 ms (DF, by the shader compiler; queue 3136 us, compile 283510 '
    'us, answer 154 us at priority 31, wake 173 us, store 225 us) after 0 ms for a compile slot: a4fe2ccba8a2dd5d'
)
HANG = (
    '__##__signpost.description#____#begin_time#_##_#{begin}##__##, '
    '__##__signpost.description#____#end_time#_##_#{end}##__##, '
    'BundleIdOverride=__##__signpost.description#____#attribute#_##_#com.apple.springboard##__## '
    'type=__##__signpost.telemetry#____#string2#_##_#Extended Launch Runloop Hang##__## enableTelemetry=YES'
)


def rec(msg: str, at: float, proc: str = 'kernel', **kw: object) -> Rec:
    """Return a record at `at` seconds after T0, its mach time on the same clock."""
    fields = {'boot': 'B', 'pid': 0, 'tid': 1, 'subsystem': '', 'category': '', 'name': '', 'kind': '', 'ident': 0}
    fields.update(kw)
    return Rec(T0 + timedelta(seconds=at), MACH0 + int(at * TICK_HZ), proc=proc, msg=msg, **fields)


def frame(template: str, at: float, origin_ticks: int, **kw: int) -> Rec:
    return rec(template.format(latch=origin_ticks + 2 * PERIOD_TICKS, origin=origin_ticks, **kw), at)


class PacingTest(unittest.TestCase):
    def test_the_stage_is_the_first_point_past_the_frame_end(self):
        origins = [MACH0 + 1000 + i * 60 * PERIOD_TICKS for i in range(6)]
        lines = (
            (22129, 4535, 17497, 16607),  # asleep in one block for a whole period: waiting, not recording
            (22700, 15000, 7600, 1500),  # on a core most of the way
            (8474, 2290, 6117, 905),  # another swap than the one the display took: not this present's line
        )
        recs = [
            # the render thread's stand-in vsync is 40 us after the frame end
            rec(RENDER.format(vsync=origin + 768, swap=s, cpu=c, blocked=b, longest=lng), i * 1.0)
            for i, (origin, (s, c, b, lng)) in enumerate(zip(origins, lines, strict=False))
        ]
        recs += [frame(LATE_RECORD, i + 0.04, origins[i]) for i in range(3)]
        recs += [frame(t, i + 3.04, origins[i + 3]) for i, t in enumerate((GAP, QUEUED, FROM_SWAP))]
        recs.sort(key=lambda r: r.time)
        stages = [s.stage for s in pacing.slow_presents(recs)]
        expected = ['render thread waited', 'record, on core', 'record', 'gap before it', 'display queue', 'unanchored']
        self.assertEqual(stages, expected)

    def test_the_commits_before_the_swap_join_the_present(self):
        origin = MACH0 + 1000
        commit = origin + int(0.020 * TICK_HZ)
        user = USER.format(commit=commit, enc=commit + 100, enc2=commit + 200, done=commit + 900)
        recs = [rec(user, 0.02, 'backboardd'), frame(LATE_RECORD, 0.04, origin)]
        slow = pacing.slow_presents(recs)[0]
        self.assertEqual([(p, cb) for p, cb, _ in slow.users], [('backboardd', 27305)])
        self.assertIn('backboardd cb 27305: commit +20.0 ms', '\n'.join(pacing.timeline(slow)))

    def test_a_cycle_is_named_by_its_longest_period(self):
        grid = int(0.5 * TICK_HZ)
        recs = [
            rec(LATE_RECORD.format(latch=MACH0 + i * grid + 3 * PERIOD_TICKS, origin=MACH0 + i * grid), i * 0.5)
            for i in range(30)
        ]
        self.assertIn('30 of 30 late presents keep to a 500 ms cycle', pacing.periodic(pacing.slow_presents(recs)))

    def test_a_short_window_takes_the_stats_windows_around_it(self):
        stats = pacing.stats_windows([rec(STATS, 5.016), rec(STATS, 10.032)])
        inside = windows.Window('test', 'pacing.list', T0, T0 + timedelta(seconds=10.5))
        brief = windows.Window('touch', 'touch', T0 + timedelta(seconds=6), T0 + timedelta(seconds=7))
        self.assertEqual(pacing.distribution(inside, stats), ([478, 14, 32, 0], 10032, 50.0, False))
        self.assertEqual(pacing.distribution(brief, stats)[0], [239, 7, 16, 0])
        self.assertTrue(pacing.distribution(brief, stats)[3])


class WindowsTest(unittest.TestCase):
    def test_a_test_window_ends_at_its_line_and_lasts_its_ms(self):
        line = rec('HWTEST display.snapshot PASS frames=12 ms=17210.4', 20.0, 'SwitchHWTest', pid=560)
        (w,) = windows.tests([line])
        self.assertEqual((w.name, w.note, round(w.ms)), ('display.snapshot', 'PASS', 17210))

    def test_a_launch_runs_from_the_first_ask_to_its_content_ready(self):
        fb = {'subsystem': 'com.apple.FrontBoard'}
        ask = 'Asked to bootstrap a new process with identity: app<com.apple.Preferences(FEEDEEEE-DDDD)>'
        recs = [
            rec(ask, 0.0, 'SpringBoard', **fb),
            rec('[app<com.apple.Preferences>:659] Bootstrap success!', 0.02, 'SpringBoard', **fb),
            rec(ask, 1.1, 'SpringBoard', **fb),
            rec(
                '[(FBSceneManager):sceneID:com.apple.Preferences-default] scene content state changed: ready',
                8.93,
                'SpringBoard',
                **fb,
            ),
        ]
        (w,) = windows.launches(recs)
        self.assertEqual((w.name, w.pid, round(w.ms)), ('com.apple.Preferences', 659, 8930))

    def test_a_hang_spans_its_signpost_begin_and_end(self):
        begin, end = MACH0 + int(2 * TICK_HZ), MACH0 + int(5.49 * TICK_HZ)
        sp = rec(HANG.format(begin=begin, end=end), 5.5, 'SpringBoard', pid=400, name='HangInterval', kind='event')
        (w,) = windows.hangs([sp])
        self.assertEqual((w.proc, w.name, w.note), ('SpringBoard', '3.49 s', 'Extended Launch Runloop Hang'))
        self.assertAlmostEqual((w.start - T0).total_seconds(), 2.0, places=3)


class WorkTest(unittest.TestCase):
    def test_creations_and_stall_signposts_cost_what_their_lines_say(self):
        stall = rec(
            f'__##__signpost.description#____#begin_time#_##_#{MACH0 + int(3.62 * TICK_HZ)}##__## seed=0x3',
            8.92,
            'Preferences',
            pid=659,
            subsystem='com.apple.coreanimation',
            category='Transaction.Stalls',
            name='Commit',
            kind='event',
        )
        costs = work.compiles([rec(MADE, 1.0, 'iconservicesagent')]) + work.stalls([stall])
        self.assertEqual(
            [(c.proc, c.what, round(c.ms)) for c in costs],
            [
                ('iconservicesagent', 'state made (compiler)', 287),
                ('Preferences', 'Core Animation transaction, begin to commit', 5300),
            ],
        )
        self.assertTrue(work.apple(costs[1]))


class CommitTest(unittest.TestCase):
    def test_a_launch_names_its_first_commit_of_either_category(self):
        sp = {'name': 'Commit', 'kind': 'event', 'subsystem': 'com.apple.coreanimation', 'pid': 659}
        begun = '__##__signpost.description#____#begin_time#_##_#{}##__## seed=0x3'
        recs = [
            rec(begun.format(MACH0 + int(3.62 * TICK_HZ)), 8.93, 'Preferences', category='Transaction.Stalls', **sp),
            rec(begun.format(MACH0 + int(9.0 * TICK_HZ)), 9.1, 'Preferences', category='Transaction', **sp),
        ]
        began, ended = work.first_commit(recs, 659, T0, T0 + timedelta(seconds=10))
        self.assertAlmostEqual((began - T0).total_seconds(), 3.62, places=3)
        self.assertEqual(ended, T0 + timedelta(seconds=8.93))


class ReportsTest(unittest.TestCase):
    def test_only_this_boots_reports_are_read_and_their_main_threads_print_with_their_waits(self):
        body = {
            'bootSessionUUID': 'b',
            'pid': 30,
            'uptime': 130,
            'termination': {'details': ['checkin with service: SpringBoard failed']},
            'stackshot': {
                'binaryImages': [['a2dca908', 0x180000000, 'S']],
                'processByPid': {
                    '30': {
                        'pid': 30,
                        'procname': 'SpringBoard',
                        'userTimeTask': 12.8,
                        'systemTimeTask': 6.4,
                        'waitInfo': ['thread 520: unknown type 22 (owner 0, context 0x1)'],
                        'threadById': {
                            '2568': {'state': ['TH_WAIT'], 'snapshotFlags': []},
                            '520': {'state': ['TH_WAIT'], 'snapshotFlags': ['kThreadMain'], 'userFrames': [[0, 0x10]]},
                        },
                    }
                },
            },
        }
        with tempfile.TemporaryDirectory() as d:
            for name, boot, build in (('SpringBoard-a.ips', 'B', '24B119'), ('SpringBoard-b.ips', 'OTHER', '21H461')):
                head = {'bug_type': '409', 'name': 'SpringBoard', 'timestamp': '2026-11-22 23:53:17'}
                with open(os.path.join(d, name), 'w') as f:
                    f.write(json.dumps(head | {'os_version': f'iPhone OS 27.2 ({build})'}))
                    f.write('\n' + json.dumps(body | {'bootSessionUUID': boot}))
            reports = ips.scan(d)
        text = '\n'.join(ips.summary(reports, {'B'}, {}, ('SpringBoard',)) + ips.watchdogs(reports))
        self.assertIn('1 reports of this boot; 1 of other boots in the folder (1 watchdog stackshot)', text)
        self.assertIn('SpringBoard[30] main thread 520: TH_WAIT', text)
        self.assertIn('thread 520: page busy (a page-in or page change in progress) (owner 0, context 0x1)', text)
        self.assertIn('dsc 0x180000010', text)
        self.assertIn('SpringBoard at 130 s uptime: main thread TH_WAIT, thread 520: page busy', text)
        self.assertIn('SpringBoard at 130 s uptime: main thread TH_WAIT, thread 520: unknown type 22', text)


class SessionTest(unittest.TestCase):
    def test_a_window_dossier_names_its_busy_cores_and_its_work(self):
        recs = [rec(POWER, 10.0), rec(MADE, 4.0, 'iconservicesagent'), rec(STATS, 5.016)]
        s = session.build('B', recs)
        w = windows.Window('launch', 'x', T0 + timedelta(seconds=3), T0 + timedelta(seconds=5))
        text = '\n'.join(session.dossier(s, w))
        self.assertIn('busy cores 1.31', text)
        self.assertIn('iconservicesagent: 1 x state made (compiler), 287 ms', text)
        self.assertIn('processes: not attributable', text)

    def test_a_window_names_the_processes_of_the_cpu_windows_ending_inside_it(self):
        s = session.build('B', [rec(CORES, 10.0), rec(TASKS, 10.001)])
        w = windows.Window('launch', 'x', T0 + timedelta(seconds=5), T0 + timedelta(seconds=11))
        self.assertIn('processes: SpringBoard 150 ms/s, backboardd 70 ms/s', '\n'.join(session.dossier(s, w)))

    def test_the_report_has_every_section_on_lines_of_any_kind(self):
        recs = [rec(POWER, 10.0), rec(STATS, 5.016), frame(LATE_RECORD, 1.0, MACH0 + 1000)]
        text = report.render(session.build('B', recs), [], {}, [])
        for heading in ('## Frame pacing', '## Slow presents', '## CPU', '## GPU', '## Hangs', '## Launches'):
            self.assertIn(heading, text)


class PullTest(unittest.TestCase):
    def test_a_pull_takes_the_log_archive_and_the_crash_folder(self):
        cmds = perf.pull_commands(perf.Path('/s'), 'abc', 600)
        self.assertEqual(cmds[0], ['idevicesyslog', '-u', 'abc', 'archive', '/s/archive.tar', '--age-limit', '600'])
        self.assertEqual(cmds[1], ['idevicecrashreport', '-u', 'abc', '-k', '-e', '/s/crash'])


class ArchiveRootTest(unittest.TestCase):
    def test_the_archive_is_found_at_the_top_or_one_folder_down(self):
        with tempfile.TemporaryDirectory() as d:
            top, nested = perf.Path(d, 'top'), perf.Path(d, 'nested')
            (nested / 'x.logarchive').mkdir(parents=True)
            top.mkdir()
            for root in (top, nested / 'x.logarchive'):
                (root / 'Info.plist').write_text('')
            self.assertEqual(perf.archive_root(top), top)
            self.assertEqual(perf.archive_root(nested), nested / 'x.logarchive')


class SourceTest(unittest.TestCase):
    def test_ndjson_lines_become_records_and_name_their_images(self):
        line = json.dumps(
            {
                'eventType': 'logEvent',
                'timestamp': '2026-11-23 00:13:20.703000+0000',
                'machTimestamp': 5,
                'bootUUID': 'B',
                'processImagePath': '/kernel.development',
                'processImageUUID': 'e181',
                'senderImagePath': '/System/Library/Extensions/X.kext/X',
                'senderImageUUID': 'abcd',
                'eventMessage': 'SwitchFrame: x',
            }
        )
        images: dict[str, str] = {}
        (r,) = source.parse_lines([line, '{"eventType": "statedumpEvent"}'], images)
        self.assertEqual((r.proc, r.msg, r.time), ('kernel', 'SwitchFrame: x', T0 + timedelta(milliseconds=703)))
        self.assertEqual(images['ABCD'], '/System/Library/Extensions/X.kext/X')


if __name__ == '__main__':
    unittest.main()
