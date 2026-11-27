"""plistlib's own XML for a set of values: the oracle switch_plistlib is tested against.

Each value is written twice as input, as a binary plist and as unsorted XML, each followed by what
`plistlib.dumps(plistlib.loads(input))` writes: the binary writer merges equal scalars (0.0 and -0.0 among them), so
the value read back is the one to compare. Every piece is prefixed by its length, a 4-byte big-endian integer.
"""

import datetime
import math
import plistlib
import struct
import sys
from pathlib import Path


def _nested(depth: int, leaf: object) -> object:
    for level in range(depth):
        leaf = [leaf] if level % 2 else {'level': leaf, 'after': level}
    return leaf


def _scalars() -> list[object]:
    return [
        'just a string',
        42,
        True,
        3.5,
        b'raw bytes',
        datetime.datetime(2001, 1, 1),
        [0, 1, -1, 255, -129, 2**63 - 1, -(2**63), 2**63, 2**64 - 1],
        [
            0.0,
            -0.0,
            1.0,
            -1.5,
            0.1,
            0.3,
            1 / 3,
            100.0,
            12345.678,
            1e15,
            1e16,
            123456789012345.6,
            1e22,
            9007199254740993.0,
            1e-4,
            1e-5,
            0.0001234,
            1.5e300,
            5e-324,
            2.2250738585072014e-308,
            1.7976931348623157e308,
            math.inf,
            -math.inf,
            math.nan,
            2.5e-7,
            6.02214076e23,
        ],
        ['', ' ', 'tab\there', 'line\nbreak', 'cr\rlf\r\nend', '& < > " \'', 'café', '日本語', '😀 astral', '\x7f'],
    ]


def _containers() -> list[object]:
    return [
        {},
        [],
        {
            'zeta': True,
            'b': 'bee',
            'ab': False,
            'a': -5,
            'B': 300,
            'a.b': ['x', 0, True, {'k': 'v'}],
            'nested': {'zz': '1', 'y': '22'},
            'emptyd': {},
            'emptya': [],
        },
        {'é': 1, 'z': 2, 'ü': 3, '~': 4, '😀': 5, '\ufffd': 6, 'A': 7, 'Ω': 8},
        {
            'com.apple.private.security.storage.AppBundles': True,
            'com.apple.private.MobileGestalt.AllowedProtectedKeys': ['UniqueDeviceID', 'ProvisioningUniqueDeviceID'],
            'fairplay-client': 2033844765,
            'seatbelt-profiles': ['installd'],
        },
        [_nested(depth, bytes(range(100))) for depth in range(12)],
        {'empty': b'', 'one': b'\x00', 'fifty-seven': bytes(range(57)), 'big': bytes(range(256)) * 3},
        [
            datetime.datetime(2020, 2, 29, 23, 59, 59),
            datetime.datetime(1969, 12, 31, 23, 59, 59),
            datetime.datetime(1960, 1, 1, 0, 0, 0, 500000),
            datetime.datetime(2038, 1, 19, 3, 14, 8),
            datetime.datetime(2001, 1, 1, 0, 0, 0, 999999),
            datetime.datetime(1900, 3, 1),
            datetime.datetime(9999, 12, 31, 23, 59, 59),
        ],
        _nested(14, 'deep'),
    ]


def main() -> None:
    """Write every case to the file named by the first argument."""
    out = bytearray()
    for value in _scalars() + _containers():
        for source in (
            plistlib.dumps(value, fmt=plistlib.FMT_BINARY, sort_keys=False),
            plistlib.dumps(value, sort_keys=False),
        ):
            for piece in (source, plistlib.dumps(plistlib.loads(source))):
                out += struct.pack('>I', len(piece)) + piece
    Path(sys.argv[1]).write_bytes(out)


if __name__ == '__main__':
    main()
