"""The built-in speaker: the kernel finds the RT5639, and audiomxd routes through the overlay's speaker strips.

Boots 15 and 16 (11-09, iOS 27) are the baseline: AudioDSPGraph refused every AID2003 speaker strip ('prs!', the
strips had no aupreset), the speaker's DSP chain failed, and VirtualAudio fell back to the null output, so nothing
played and the volume controls were greyed out. Boot 17 routed the speaker, but every boot since 11-08 found no codec
(NACK at I2C1 0x1c: SwitchAudio read it before PZ4's pad drove LDO1_EN), so the kernel ran the null backend.
"""

from tools.devicerun.rules import REPORT, Check

OWNER = 'r124-audio27'
AUDIOMXD = 'audiomxd'
KERNEL = 'kernel'
STRIP = "DSP graph '*.austrip'"
RT5639 = 'drivers/SwitchAudio/SwitchRT5639.cpp'

AUDIO = [
    Check(
        'audio.codec-ready',
        OWNER,
        'Tegra I2S1 + RT5639 ready',
        'count >= 1',
        process=KERNEL,
        source='drivers/SwitchAudio/SwitchAudio.cpp',
        note='the Tegra backend: boots since 11-08 never logged it',
    ),
    Check(
        'audio.codec-absent',
        OWNER,
        'no RT5639 on I2C1',
        process=KERNEL,
        regex=r'VENDOR_ID2 (?P<id>\w+), error (?P<error>-?\d+)',
        show=('error',),
        source=RT5639,
        note='boots 15 to 17: error -4 (NACK) every boot, the null backend',
    ),
    Check(
        'audio.speaker-strip',
        OWNER,
        f'setting {STRIP}',
        REPORT,
        process=AUDIOMXD,
        regex=r'\|(?P<chain>speaker_\w+)\] setting',
        show=('chain',),
        source='AudioDSPGraph (dyld cache)',
        note='the speaker chains audiomxd built',
    ),
    Check(
        'audio.strip-refused',
        OWNER,
        f'failed to set {STRIP}',
        process=AUDIOMXD,
        regex=r'\|(?P<chain>\w+)\] failed to set .*CADSPErrorStatus=(?P<status>-?\d+)',
        show=('chain', 'status'),
        needs='audio.speaker-strip',
        source='AudioDSPGraph (dyld cache)',
        note="boots 15 and 16: speaker_general, status 1886548769 ('prs!')",
    ),
    Check(
        'audio.routing-not-supported',
        OWNER,
        'Routing is not supported',
        process=AUDIOMXD,
        needs='audio.speaker-strip',
        source='VirtualAudio RoutingManager.cpp',
        note='every route activation after the speaker chain failed; boots 15 and 16',
    ),
]
