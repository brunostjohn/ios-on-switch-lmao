"""apps.py's checks over boot 16b's lsd and SpringBoard lines (11-09, iOS 27), its second boot after a rebuild."""

import unittest

from tools.devicerun import devicerun
from tools.devicerun.apps import APPS
from tools.devicerun.rules import FAIL, NO_DATA, PASS

FIRST_BOOT = 'Reboot detected, performing first boot work.'
REMOVED = 'Marking container at /Applications/ for removal'
NOT_INSTALLED = (
    'bad app layout found in persistent storage. one of the referenced apps is not installed.  appLayout=<SBAppLayout: '
    '0x8abb3e4c0; primary: com.brunostjohn.switch.hwtest:default; environment: main> {'
)
APPLE_NOT_INSTALLED = NOT_INSTALLED.replace('com.brunostjohn.switch.hwtest', 'com.apple.Maps')


def record(process: str, message: str, second: int = 0) -> dict:
    return {
        'timestamp': f'2026-11-09 03:48:{second:02d}.000000-0800',
        'bootUUID': '88A60631',
        'processImagePath': f'/usr/libexec/{process}',
        'subsystem': 'com.apple.launchservices',
        'category': 'default',
        'messageType': 'Default',
        'eventMessage': message,
    }


def results(*records: dict) -> dict:
    found, _ = devicerun.run_checks(list(records), APPS)
    return {r.check.name: r for r in found}


class OverlayApps(unittest.TestCase):
    def test_boot_16b_loses_the_overlay_apps(self):
        got = results(record('lsd', FIRST_BOOT), record('lsd', REMOVED, 3), record('SpringBoard', NOT_INSTALLED, 54))
        self.assertEqual(got['apps.overlay-container-removed'].verdict, FAIL)
        self.assertEqual(got['apps.overlay-not-installed'].verdict, FAIL)
        self.assertIn('com.brunostjohn.switch.hwtest', got['apps.overlay-not-installed'].observed)

    def test_a_boot_that_keeps_them_passes(self):
        got = results(record('lsd', FIRST_BOOT), record('SpringBoard', APPLE_NOT_INSTALLED, 54))
        self.assertEqual(got['apps.overlay-container-removed'].verdict, PASS)
        self.assertEqual(got['apps.overlay-not-installed'].verdict, PASS)

    def test_no_first_boot_work_is_no_data(self):
        self.assertEqual(results()['apps.overlay-container-removed'].verdict, NO_DATA)


if __name__ == '__main__':
    unittest.main()
