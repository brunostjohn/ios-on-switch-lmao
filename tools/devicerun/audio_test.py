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
NO_CODEC = 'SwitchAudio: no RT5639 on I2C1 0x1c (VENDOR_ID2 0000, error -4): audio is the null backend'
CODEC_READY = (
    'SwitchAudio: Tegra I2S1 + RT5639 ready (playback and capture): S32_LE, I2S 32-bit slots at 64 fs (3072 kHz BCLK), '
    'RT5639 24-bit, 47999.878 Hz'
)


def record(message: str, second: int = 0, process: str = '/usr/sbin/audiomxd') -> dict:
    return {
        'timestamp': f'2026-11-09 03:39:{second:02d}.000000-0800',
        'bootUUID': '5D1A0C3E',
        'processImagePath': process,
        'subsystem': 'com.apple.coreaudio',
        'category': '',
        'messageType': 'Error',
        'eventMessage': message,
    }


KERNEL = '/System/Library/Caches/com.apple.kernelcaches/kernel.development'


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

    def test_boot_17_found_no_codec(self):
        got = results(record(NO_CODEC, process=KERNEL))
        self.assertEqual(got['audio.codec-ready'].verdict, FAIL)
        self.assertEqual(got['audio.codec-absent'].verdict, FAIL)
        self.assertIn('-4', got['audio.codec-absent'].observed)

    def test_a_found_codec_passes(self):
        got = results(record(CODEC_READY, process=KERNEL))
        self.assertEqual(got['audio.codec-ready'].verdict, PASS)
        self.assertEqual(got['audio.codec-absent'].verdict, PASS)

    def test_no_speaker_chain_is_no_data(self):
        got = results()
        self.assertEqual(got['audio.strip-refused'].verdict, NO_DATA)
        self.assertEqual(got['audio.routing-not-supported'].verdict, NO_DATA)


if __name__ == '__main__':
    unittest.main()
