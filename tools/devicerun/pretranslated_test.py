"""pretranslated.py's checks over the kernel's deploy-update lines."""

import unittest

from tools.devicerun import devicerun
from tools.devicerun.pretranslated import PRETRANSLATED
from tools.devicerun.rules import FAIL, PASS

KERNEL = '/System/Library/Kernels/kernel.development'


def kernel(message: str, second: int = 0) -> dict:
    return {
        'timestamp': f'2026-11-26 10:00:{second:02d}.000000+0000',
        'bootUUID': 'B',
        'processImagePath': KERNEL,
        'subsystem': '',
        'category': '',
        'messageType': 'Default',
        'eventMessage': message,
    }


def results(*records: dict) -> dict:
    found, _ = devicerun.run_checks(list(records), PRETRANSLATED)
    return {r.check.name: r for r in found}


class Pretranslated(unittest.TestCase):
    def test_an_update_taken_passes(self):
        got = results(kernel("SwitchPretranslated: /usr/libexec/locationd from the deploy's update"))
        for name, result in got.items():
            self.assertEqual(result.verdict, PASS, name)

    def test_any_other_line_for_a_file_fails(self):
        for line in (
            'SwitchPretranslated: /usr/libexec/locationd not updated (not signed as its CDHash); it runs translated '
            'at load time',
            'SwitchPretranslated: /usr/libexec/locationd not updated (not in the tree)',
            "SwitchPretranslated: /usr/libexec/locationd from the deploy's update, eventually",
        ):
            with self.subTest(line=line):
                self.assertEqual(results(kernel(line))['pretranslated.update'].verdict, FAIL)
        got = results(kernel('SwitchPretranslated: 1830 files from /private/var/Switch/pretranslated (fsid 1:2)'))
        self.assertEqual(got['pretranslated.update'].verdict, PASS, "the tree's own line names no update")

    def test_an_unusable_update_folder_fails_unless_it_is_only_missing(self):
        missing = (
            'SwitchPretranslated: 1 of 1 updates needed, none applied (/private/var/Switch/pretranslated-updates: '
        )
        self.assertEqual(results(kernel(missing + 'no such file)'))['pretranslated.update-folder'].verdict, PASS)
        owner = (
            'SwitchPretranslated: 1 of 1 updates needed, none applied (/private/var/Switch: not root:wheel, or '
            'writable by group or others)'
        )
        self.assertEqual(results(kernel(owner))['pretranslated.update-folder'].verdict, FAIL)
        self.assertEqual(results(kernel(missing + 'unreadable)'))['pretranslated.update-folder'].verdict, FAIL)

    def test_an_unreadable_update_list_fails(self):
        got = results(kernel('SwitchPretranslated: no updates (/chosen/switch-pretranslated-updates: 6)'))
        self.assertEqual(got['pretranslated.update-list'].verdict, FAIL)


if __name__ == '__main__':
    unittest.main()
