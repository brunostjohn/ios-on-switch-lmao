"""motion.py's checks over boot 2's CoreMotion lines (22-23 Nov, iOS 27)."""

import unittest

from tools.devicerun import devicerun
from tools.devicerun.motion import MOTION
from tools.devicerun.rules import FAIL, INFO, NA, NO_DATA, PASS

LOCATIOND = '/usr/libexec/locationd'
BACKBOARDD = '/usr/libexec/backboardd'
SPRINGBOARD = '/System/Library/CoreServices/SpringBoard.app/SpringBoard'
HWTEST = '/private/var/containers/Bundle/Application/X/SwitchHWTest.app/SwitchHWTest'
KERNEL = '/System/Library/Kernels/kernel.development'


def record(message: str, process: str, second: int = 0, boot: str = 'B2') -> dict:
    return {
        'timestamp': f'2026-11-22 23:53:{second:02d}.000000+0000',
        'bootUUID': boot,
        'processImagePath': process,
        'subsystem': 'com.apple.locationd.Motion',
        'category': 'Motion',
        'messageType': 'Default',
        'eventMessage': message,
    }


def results(*records: dict) -> dict:
    found, _ = devicerun.run_checks(list(records), MOTION)
    return {r.check.name: r for r in found}


def darwin(major: int, boot: str = 'B2') -> dict:
    return record(
        f'Darwin Kernel Version {major}.5.0: Thu  1 Jan 1970 00:00:00 UTC; switch-ios:ios-obj/X', KERNEL, 0, boot
    )


def at(second: float, message: str, process: str) -> dict:
    return {**record(message, process), 'timestamp': f'2026-11-22 23:53:{second:09.6f}+0000'}


IOS27, IOS17 = darwin(25), darwin(23)
START = '[CLIoHidInterface] Property for usage pair {65292, 5}: {ReportInterval = 10000} was set successfully'
COPY = '[FastPath] copyEventPrivate() Failed (e00002c0)'
OPEN = '[FastPath] openFastPathPrivate() failed,serviceRef,<IOHIDServiceClient>'


BOOT_2 = (
    record('[CLIoHidInterface] Adding new Device with usage pair {65292, 26}', BACKBOARDD, 2),
    record('[CLIoHidInterface] Service for device with usage pair {65292, 26} is not ready', BACKBOARDD, 3),
    record('[CLIoHidInterface] Could not set all properties after too many retries. Giving up.', BACKBOARDD, 4),
    record('[CLIoHidInterface] Service for usage pair {65292, 5} is not ready', HWTEST, 5),
    record('[FastPath] Failed to open because of null service ref', SPRINGBOARD, 6),
    record('[CLIoHidInterface] Could not set all properties after too many retries. Giving up.', SPRINGBOARD, 7),
    record('[CLIoHidInterface] Adding new Device with usage pair {65292, 8}', LOCATIOND, 8),
)


class Motion(unittest.TestCase):
    def test_boot_2_asked_for_the_coprocessor(self):
        got = results(*BOOT_2)
        for name in (
            'motion.spu-added',
            'motion.spu-not-ready',
            'motion.give-up.backboardd',
            'motion.give-up.SpringBoard',
            'motion.fastpath-null',
        ):
            self.assertEqual(got[name].verdict, FAIL, name)
        self.assertEqual(got['motion.spu-added-ungated'].verdict, INFO)

    def test_locationds_own_table_is_judged_on_ios_27_only(self):
        locationd = (
            record('[CLIoHidInterface] Adding new Device with usage pair {65292, 1}', LOCATIOND),
            record('[CLIoHidInterface] Service for device with usage pair {65292, 5} is not ready', LOCATIOND, 1),
            record('[CLIoHidInterface] Could not set all properties after too many retries. Giving up.', LOCATIOND, 2),
        )
        names = ('motion.locationd-spu-added', 'motion.locationd-spu-not-ready', 'motion.give-up.locationd')
        got = results(IOS27, *locationd)
        for name in names:
            self.assertEqual(got[name].verdict, FAIL, name)
        self.assertEqual(got['motion.spu-added'].verdict, PASS, "the cache's copy is judged apart")
        self.assertEqual(got['motion.spu-not-ready'].verdict, PASS)
        got = results(IOS17, *locationd)
        for name in names:
            self.assertEqual(got[name].verdict, NA, name)
        got = results(*locationd)
        for name in names:
            self.assertEqual(got[name].verdict, NO_DATA, f'{name}: an archive that starts after the boot')

    def test_an_archive_of_two_boots_judges_each_its_way(self):
        failing = record(
            '[CLIoHidInterface] Service for device with usage pair {65292, 5} is not ready', LOCATIOND, 9, 'A'
        )
        got = results(darwin(23, 'A'), failing, darwin(25, 'B'))
        self.assertEqual(got['motion.locationd-spu-not-ready'].verdict, PASS, "the iOS 17 boot's line is not judged")
        on_27 = record(
            '[CLIoHidInterface] Service for device with usage pair {65292, 5} is not ready', LOCATIOND, 9, 'B'
        )
        got = results(darwin(23, 'A'), darwin(25, 'B'), on_27)
        self.assertEqual(got['motion.locationd-spu-not-ready'].verdict, FAIL)

    def test_locationds_device_motion_is_switchmotions(self):
        got = results(IOS27, record('[CLIoHidInterface] Adding new Device with usage pair {65292, 5}', LOCATIOND))
        self.assertEqual(got['motion.locationd-spu-added'].verdict, PASS)
        self.assertEqual(got['motion.locationd-spu-not-ready'].verdict, PASS)

    def test_switchmotions_device_motion_service_passes(self):
        got = results(
            record('[CLIoHidInterface] Adding new Device with usage pair {65292, 5}', SPRINGBOARD),
            record(
                '[CLIoHidInterface] Property for usage pair {65292, 5}: {ReportInterval = 10000} was set successfully',
                SPRINGBOARD,
                1,
            ),
        )
        self.assertEqual(got['motion.spu-added'].verdict, PASS)
        self.assertEqual(got['motion.spu-not-ready'].verdict, PASS)
        self.assertEqual(got['motion.device-motion-interval'].verdict, INFO)
        self.assertEqual(got['motion.device-motion-rejected'].verdict, PASS)

    def test_a_fast_path_coremotion_cannot_open_fails_where_the_sandbox_admits_it(self):
        for process, name in ((SPRINGBOARD, 'SpringBoard'), (LOCATIOND, 'locationd')):
            self.assertEqual(results(IOS27, record(OPEN, process))[f'motion.fastpath-open.{name}'].verdict, FAIL, name)
        self.assertEqual(results(IOS17, record(OPEN, LOCATIOND))['motion.fastpath-open.locationd'].verdict, NA)
        self.assertEqual(results(IOS27, record(OPEN, BACKBOARDD))['motion.fastpath-open.backboardd'].verdict, INFO)

    def test_a_copy_without_a_report_is_expected_only_as_its_process_starts_a_session(self):
        got = results(IOS27, record(START, SPRINGBOARD), at(0.5, COPY, SPRINGBOARD))
        self.assertEqual(got['motion.fastpath-copy.SpringBoard'].verdict, PASS)
        got = results(IOS27, record(START, SPRINGBOARD), record(COPY, SPRINGBOARD, 5))
        self.assertEqual(got['motion.fastpath-copy.SpringBoard'].verdict, FAIL)
        got = results(IOS27, record(START, SPRINGBOARD), at(0.5, COPY, LOCATIOND))
        self.assertEqual(got['motion.fastpath-copy.locationd'].verdict, FAIL, "another process's start excuses nothing")
        self.assertEqual(got['motion.fastpath-copy'].verdict, INFO)

    def test_backboardds_refused_fast_path_and_its_failed_copies_are_reported_only(self):
        got = results(IOS27, record(START, BACKBOARDD), record(OPEN, BACKBOARDD, 1), record(COPY, BACKBOARDD, 9))
        self.assertEqual(got['motion.fastpath-open.backboardd'].verdict, INFO)
        self.assertEqual(got['motion.fastpath-copy'].verdict, INFO)
        self.assertNotIn(FAIL, {r.verdict for r in got.values()})

    def test_a_report_coremotion_refuses_fails(self):
        got = results(record('Bad device motion report,type,3,size,52', SPRINGBOARD))
        self.assertEqual(got['motion.device-motion-rejected'].verdict, FAIL)

    def test_the_accelerometer_and_gyroscope_alone_pass(self):
        got = results(
            IOS27,
            record('[CLIoHidInterface] Adding new Device with usage pair {65280, 3}', SPRINGBOARD),
            record(
                '[CLIoHidInterface] Property for usage pair {65280, 9}: {ReportInterval = 10000} was set', HWTEST, 1
            ),
        )
        for name, result in got.items():
            if name not in (
                'motion.kernel',
                'motion.ios27-boot',
                'motion.spu-added-ungated',
                'motion.device-motion-interval',
                'motion.fastpath-open.backboardd',
                'motion.fastpath-copy',
            ):
                self.assertEqual(result.verdict, PASS, name)


if __name__ == '__main__':
    unittest.main()
