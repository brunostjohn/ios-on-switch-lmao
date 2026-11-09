"""audio.py's checks over boot 16's audiomxd lines (11-09, iOS 27)."""

import unittest

from tools.devicerun import devicerun
from tools.devicerun.audio import AUDIO
from tools.devicerun.rules import FAIL, INFO, NO_DATA, PASS

SETTING = "[0xd49838040|speaker_general] setting DSP graph '*.austrip' at index 0"
REFUSED = (
    "[0xd49838040|speaker_general] failed to set DSP graph '*.austrip' at index 0 - Error Domain="
    'com.apple.audio.AudioDSPGraph Code=1702388592 "failed to set parameter strip" UserInfo={NSLocalizedDescription='
    'failed to set parameter strip, CADSPErrorSourceLine=261, CADSPErrorSourceFile=/Library/Caches/com.apple.xbs/'
    'Sources/AudioDSPGraph/Targets/Framework/AudioDSPGraph/CADSPGraph.mm, CADSPErrorStatus=1886548769}'
)
NOT_SUPPORTED = 'RoutingManager.cpp:3687  Routing is not supported: attempt to activate the routes failed.'


def record(message: str, second: int = 0) -> dict:
    return {
        'timestamp': f'2026-11-09 03:39:{second:02d}.000000-0800',
        'bootUUID': '5D1A0C3E',
        'processImagePath': '/usr/sbin/audiomxd',
        'subsystem': 'com.apple.coreaudio',
        'category': '',
        'messageType': 'Error',
        'eventMessage': message,
    }


def results(*records: dict) -> dict:
    found, _ = devicerun.run_checks(list(records), AUDIO)
    return {r.check.name: r for r in found}


class Audio(unittest.TestCase):
    def test_boot_16_fails_the_speaker_route(self):
        got = results(record(SETTING), record(REFUSED), record(NOT_SUPPORTED, second=1))
        self.assertEqual(got['audio.speaker-strip'].verdict, INFO)
        self.assertEqual(got['audio.strip-refused'].verdict, FAIL)
        self.assertIn('1886548769', got['audio.strip-refused'].observed)
        self.assertEqual(got['audio.routing-not-supported'].verdict, FAIL)

    def test_an_applied_strip_passes(self):
        got = results(record(SETTING))
        self.assertEqual(got['audio.strip-refused'].verdict, PASS)
        self.assertEqual(got['audio.routing-not-supported'].verdict, PASS)

    def test_no_speaker_chain_is_no_data(self):
        got = results()
        self.assertEqual(got['audio.strip-refused'].verdict, NO_DATA)
        self.assertEqual(got['audio.routing-not-supported'].verdict, NO_DATA)


if __name__ == '__main__':
    unittest.main()
