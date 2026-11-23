"""springboard.py's first-life verdict over lines in 24B119's formats (boot 2, 22 Nov)."""

import unittest
from datetime import datetime, timedelta

from tools.devicerun import devicerun, springboard
from tools.devicerun.rules import FAIL, INFO, NO_DATA, PASS

BOOT = '3349B3E8-362E-46C3-A562-EB07470C449B'
BOOT2 = '9911A4C2-362E-46C3-A562-EB07470C449B'
WALL = datetime(2026, 11, 22, 23, 51, 5, 251000)
CLIENT = (
    'checkIn: <BKSystemShellClient: 0xba32e2220; SpringBoard.{pid}; bundleIdentifier: com.apple.springboard; '
    'bundlePath: /System/Library/CoreServices/SpringBoard.app; jobLabel: com.apple.SpringBoard; '
    'systemIdleSleepInterval: 1.00; watchdogType: shell> {{\n    pendingCheckIn: YES;\n}}'
)


def record(process: str, subsystem: str, message: str, uptime: float | None, wall_step: float = 0.0) -> dict:
    """Return a line at `uptime` s of mach time (omitting machTimestamp when None), wall time off by `wall_step`."""
    wall = WALL + timedelta(seconds=(uptime or 0.0) + wall_step)
    out = {
        'timestamp': wall.strftime('%Y-%m-%d %H:%M:%S.%f') + '+0000',
        'bootUUID': BOOT,
        'processImagePath': f'/System/Library/CoreServices/{process}',
        'subsystem': subsystem,
        'category': 'SystemShell',
        'messageType': 'Default',
        'eventMessage': message,
    }
    if uptime is not None:
        out['machTimestamp'] = int(uptime * springboard.MACH_HZ)
    return out


def window_ready(uptime: float | None) -> dict:
    return record('backboardd', 'com.apple.BackBoard', '      window server ready in 4.2000s. overall 18.0000s', uptime)


def checkin(pid: int, uptime: float | None, wall_step: float = 0.0) -> dict:
    return record('backboardd', 'com.apple.BackBoard', CLIENT.format(pid=pid), uptime, wall_step)


def finished(uptime: float | None, wall_step: float = 0.0) -> dict:
    return record('SpringBoard', 'com.apple.BackBoard', 'checkin finishedLaunching', uptime, wall_step)


def logo(pid: int, uptime: float | None) -> dict:
    return record('backboardd', 'com.apple.BackBoard', f'Dismissing boot logo (pid:{pid})', uptime)


def exited(pid: int, uptime: float | None) -> dict:
    return record('launchd', f'user/501/com.apple.SpringBoard [{pid}]', 'job state = exited', uptime)


def on_boot(boot: str, *records: dict) -> list[dict]:
    return [{**r, 'bootUUID': boot} for r in records]


def verdict(*records: dict):
    results, _ = devicerun.run_checks(list(records), springboard.SPRINGBOARD)
    (row,) = springboard.springboard_rows({r.check.name: r for r in results})
    return row


class FirstLife(unittest.TestCase):
    def test_boot_2s_first_springboard_was_killed_and_the_second_dismissed_the_logo(self):
        row = verdict(
            window_ready(53.700),
            checkin(30, 57.820),
            exited(30, 132.558),
            checkin(400, 133.615),
            logo(400, 154.059),
            finished(154.932),
        )
        self.assertEqual(row.verdict, FAIL)
        self.assertIn(
            'SpringBoard[30] never finished launching (SpringBoard[400] checked in 75.8 s later)', row.observed
        )
        self.assertIn('SpringBoard[400] check-in to finishedLaunching 21.3 s', row.observed)
        self.assertIn('SpringBoard exited 1 time(s) before the logo', row.observed)
        self.assertIn('logo by SpringBoard[400]', row.observed)

    def test_a_first_life_inside_the_watchdog_passes_across_a_wall_clock_step(self):
        # 40 s of mach time; the wall clock stepped 35 s forward between the two lines, so it reads 75 s.
        row = verdict(window_ready(18.0), checkin(30, 22.0), logo(30, 60.0), finished(62.0, wall_step=35.0))
        self.assertEqual(row.verdict, PASS, row.observed)
        self.assertIn('SpringBoard[30] check-in to finishedLaunching 40.0 s', row.observed)

    def test_a_first_life_finishing_past_the_watchdog_fails(self):
        row = verdict(window_ready(18.0), checkin(30, 22.0), logo(30, 90.0), finished(92.5))
        self.assertEqual(row.verdict, FAIL)
        self.assertIn('check-in to finishedLaunching 70.5 s', row.observed)

    def test_a_springboard_that_dies_before_checking_in_fails_the_boot(self):
        row = verdict(window_ready(18.0), exited(30, 40.0), checkin(77, 45.0), logo(77, 60.0), finished(61.0))
        self.assertEqual(row.verdict, FAIL)
        self.assertIn('SpringBoard exited 1 time(s) before the logo', row.observed)

    def test_no_logo_fails(self):
        row = verdict(window_ready(18.0), checkin(30, 22.0), finished(30.0))
        self.assertEqual(row.verdict, FAIL)
        self.assertIn('no logo', row.observed)

    def test_no_checkin_line_is_no_data(self):
        row = verdict(logo(30, 60.0))
        self.assertEqual(row.verdict, NO_DATA)
        post_logo_exit = verdict(logo(30, 60.0), exited(30, 300.0))
        self.assertEqual(post_logo_exit.verdict, NO_DATA, post_logo_exit.observed)

    def test_truncated_start_past_first_kill_does_not_pass(self):
        row = verdict(checkin(400, 133.615), logo(400, 154.059), finished(154.932))
        self.assertEqual(row.verdict, NO_DATA, row.observed)
        self.assertIn('truncated start', row.observed)
        # A window-server-ready line after the first check-in does not anchor it either.
        after = verdict(checkin(400, 133.615), window_ready(140.0), logo(400, 154.059), finished(154.932))
        self.assertEqual(after.verdict, NO_DATA, after.observed)

    def test_missing_mach_timestamp_does_not_pass_a_killed_boot(self):
        row = verdict(window_ready(None), exited(30, None), checkin(400, None), logo(400, None), finished(None))
        self.assertEqual(row.verdict, NO_DATA, row.observed)
        self.assertIn('missing machTimestamp', row.observed)

    def test_multi_boot_archive_where_an_earlier_boot_dies_before_checkin_fails(self):
        row = verdict(
            exited(30, 40.0),
            *on_boot(BOOT2, window_ready(18.0), checkin(30, 22.0), logo(30, 40.0), finished(42.0)),
        )
        self.assertEqual(row.verdict, FAIL, row.observed)
        self.assertIn('without checking in', row.observed)
        only_dead = verdict(exited(30, 40.0))
        self.assertEqual(only_dead.verdict, FAIL, only_dead.observed)

    def test_duplicate_checkin_for_the_same_pid_uses_the_earliest_timestamp(self):
        ok = verdict(window_ready(18.0), checkin(30, 22.0), checkin(30, 25.0), logo(30, 40.0), finished(42.0))
        self.assertEqual(ok.verdict, PASS, ok.observed)
        self.assertIn('SpringBoard[30] check-in to finishedLaunching 20.0 s', ok.observed)
        late = verdict(window_ready(18.0), checkin(30, 22.0), checkin(30, 60.0), logo(30, 90.0), finished(93.0))
        self.assertEqual(late.verdict, FAIL, late.observed)
        self.assertIn('SpringBoard[30] check-in to finishedLaunching 71.0 s', late.observed)


def kernel(message: str, uptime: float) -> dict:
    return {**record('kernel', '', message, uptime), 'processImagePath': '/kernel.development'}


# Each kernel and driver line as its format string writes it.
OBSERVED = {
    'boot.stall-chain': kernel(
        "SwitchStall: backboardd[69] main thread 0x264 chain: the first owner's kernel rwlock is held for writing by "
        'kernel_task[0] thread 0x1c6 apfs_transaction_flusher: blocked (pri 81): page busy on wait queue 0x1, event '
        '0x2; its kernel 0xfffffff0074fae5c;',
        39.6,
    ),
    'boot.steps': kernel(
        'SwitchBootSteps: cryptex setup (ms): Preboot 21, directories 3, System cryptex 140, App cryptex 80, '
        'auth fs 1, cryptex unions 610, boot manifest 290',
        4.2,
    ),
    'boot.bluetooth-before-launchd': kernel(
        'SwitchBluetooth: before launchd: staged address read in 30 ms, the Joy-Con rails told it in 900 ms', 7.1
    ),
    'boot.tasks': kernel(
        'SwitchBootTask: fsck[2] exited at 10.402 s of uptime: CPU 120 ms user + 40 ms system, 312 page-ins', 10.4
    ),
    'boot.metal-open-split': {
        **record(
            'backboardd',
            'com.brunostjohn.switch.TegraGM20BMetal',
            'SwitchMetal: open split in backboardd (ms): cache directory 3, device 1290 (of it waiting for the prepare '
            'thread 1100, which took open 900 and warm 380), states 70',
            35.5,
        ),
        'processImagePath': '/usr/libexec/backboardd',
    },
    'boot.keystore-slow': kernel('AppleKeyStore: selector 11 from usermanagerd[56] took 1600 ms (0x0)', 22.3),
}


class Observations(unittest.TestCase):
    def test_each_line_is_its_rows_and_only_its(self):
        results, _ = devicerun.run_checks(list(OBSERVED.values()), springboard.OBSERVATIONS)
        for r in results:
            self.assertEqual(len(r.matches), 1, r.check.name)
            self.assertEqual(r.matches[0]['message'], OBSERVED[r.check.name]['eventMessage'], r.check.name)
            self.assertEqual(r.verdict, INFO, r.check.name)

    def test_a_stall_line_without_a_chain_is_not_a_chain_row(self):
        first = kernel('SwitchStall: SpringBoard[30] main thread 0x208 blocked 2.2 s (since 59.9 s of uptime)', 62.2)
        results, _ = devicerun.run_checks([first], springboard.OBSERVATIONS)
        self.assertEqual({r.check.name: r.verdict for r in results}['boot.stall-chain'], NO_DATA)

    def test_the_slow_keystore_row_names_the_caller(self):
        results, _ = devicerun.run_checks([OBSERVED['boot.keystore-slow']], springboard.OBSERVATIONS)
        row = {r.check.name: r for r in results}['boot.keystore-slow']
        self.assertIn('usermanagerd x1', row.observed)
        unsupported = kernel('AppleKeyStore: selector 42 unsupported (first from keybagd)', 14.0)
        results, _ = devicerun.run_checks([unsupported], springboard.OBSERVATIONS)
        self.assertEqual({r.check.name: r.verdict for r in results}['boot.keystore-slow'], NO_DATA)


if __name__ == '__main__':
    unittest.main()
