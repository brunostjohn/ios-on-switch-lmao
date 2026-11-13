"""The HW Test apps hold nothing of the Mac that built them, and the iOS 27 app built twice is the same bytes.

The twin is the app in a configuration of its own (apps.bzl app_twin), so each of its actions ran again, in other
sandboxes and output directories: what differs between them is what an app rebuilt anywhere could differ by, and a
rebuilt app whose CDHash is not the boot package's trust cache entry does not launch.
"""

import os
import plistlib
import unittest
import zipfile
from pathlib import Path

# Info.plist keys of the build machine rather than of the inputs (rules_apple's environment plist)
HOST_KEYS = {'BuildMachineOSBuild'}


def members(ipa: str) -> dict[str, bytes]:
    with zipfile.ZipFile(ipa) as archive:
        return {info.filename: archive.read(info) for info in archive.infolist()}


class Reproducible(unittest.TestCase):
    def test_the_twin_is_the_same_app(self):
        app, twin = members(os.environ['APP']), members(os.environ['TWIN'])
        self.assertEqual(sorted(app), sorted(twin))
        self.assertEqual([name for name in app if app[name] != twin[name]], [])
        self.assertEqual(Path(os.environ['APP']).read_bytes(), Path(os.environ['TWIN']).read_bytes())

    def test_no_app_names_its_build_machine(self):
        for ipa in os.environ['APPS'].split():
            for name, data in members(ipa).items():
                if name.endswith('.app/Info.plist'):
                    with self.subTest(app=name):
                        self.assertEqual(HOST_KEYS & set(plistlib.loads(data)), set())


if __name__ == '__main__':
    unittest.main()
