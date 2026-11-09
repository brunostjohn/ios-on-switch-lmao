"""The built-in speaker's route: audiomxd applies the overlay's speaker strips and activates the route through them.

Boots 15 and 16 (11-09, iOS 27) are the baseline: AudioDSPGraph refused every AID2003 speaker strip ('prs!', the
strips had no aupreset), the speaker's DSP chain failed, and VirtualAudio fell back to the null output, so nothing
played and the volume controls were greyed out.
"""

from tools.devicerun.rules import REPORT, Check

OWNER = 'r124-audio27'
AUDIOMXD = 'audiomxd'
STRIP = "DSP graph '*.austrip'"

AUDIO = [
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
