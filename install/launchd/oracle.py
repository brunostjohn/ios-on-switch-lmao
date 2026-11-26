"""The oracle switch_launchd is tested against: plistlib's binary round trip and launchd_catalog's edit.

Records follow one another: a kind byte (0 a round trip, 1 an edit), an outcome byte (0 the expected bytes follow, 1
Python refused), then pieces, each prefixed by its length as a 4-byte big-endian integer. A round trip's pieces are the
source and `dumps(loads(source, fmt=FMT_BINARY), fmt=FMT_BINARY, sort_keys=False)`; an edit's are the catalogue, the
three job plists and `add_bootstrap_jobs`' result. A refused case's last piece is empty.

Arguments: the output file, then the bootstrap, bootstrap-apps and sdwake job plists.
"""

import datetime
import math
import plistlib
import random
import struct
import sys
from pathlib import Path

from install.launchd.edits import edit_cases
from install.launchd.handmade import bplist, handmade_sources

REFUSALS = (ValueError, TypeError, KeyError, AttributeError, OverflowError)


def _binary(value: object) -> bytes:
    return plistlib.dumps(value, fmt=plistlib.FMT_BINARY, sort_keys=False)


def _round_trip(source: bytes) -> bytes | None:
    try:
        return _binary(plistlib.loads(source, fmt=plistlib.FMT_BINARY))
    except REFUSALS:
        return None


def _numbers() -> list[object]:
    nan = math.nan
    payload = struct.unpack('>d', bytes.fromhex('7ff0000000000123'))[0]
    negative = struct.unpack('>d', bytes.fromhex('fff8000000000000'))[0]
    ints = [0, 1, 255, 256, 65535, 65536, 2**32 - 1, 2**32, 2**63 - 1, 2**63, 2**64 - 1, -1, -255, -(2**63)]
    return [
        ints,
        [True, 1, 1.0, False, 0, 0.0, -0.0, None, None],
        [0.0, -0.0],
        [-0.0, 0.0, 0.0],
        [nan, nan],
        [float('nan'), float('nan')],
        [payload, payload, negative, math.inf, -math.inf, 5e-324, 1.5],
        {'n': nan, 'm': [nan], 'o': float('nan')},
    ]


def _texts() -> list[object]:
    sizes = [14, 15, 16, 255, 256, 65535, 65536]
    return [
        ['', 'a', 'café', '日本語', '😀 astral', '\x00nul', 'a' * 14, 'é' * 14, 'é' * 15, 'é' * 300],
        ['x' * size for size in sizes],
        [b'', b'\x00', *(bytes(size) for size in sizes)],
        [plistlib.UID(value) for value in (0, 255, 256, 65535, 65536, 2**32 - 1, 2**32, 2**64 - 1)],
        *(
            [moment, moment.replace(microsecond=0)]
            for moment in (
                datetime.datetime(2001, 1, 1, 0, 0, 0, 1),
                datetime.datetime(1969, 12, 31, 23, 59, 59, 999999),
                datetime.datetime(1, 1, 1),
                datetime.datetime(9999, 12, 31, 23, 59, 59, 999999),
                datetime.datetime(2300, 6, 1, 12, 0, 0, 123457),
                datetime.datetime(1700, 3, 1, 0, 0, 0, 500000),
            )
        ),
    ]


def _containers() -> list[object]:
    shared = {'a': 1, 'b': [1, 2]}
    uid = plistlib.UID(7)
    cycle: list[object] = []
    cycle.append(cycle)
    looped: dict[str, object] = {'k': 'v'}
    looped['self'] = looped
    nested: object = 'leaf'
    for level in range(40):
        nested = [nested] if level % 2 else {'level': nested, 'k': level}
    return [
        {},
        [],
        {'x': shared, 'y': [shared, shared], 'z': {'w': shared}},
        [uid, uid, plistlib.UID(7)],
        cycle,
        looped,
        nested,
        {'zeta': 1, 'alpha': 2, 'mid': {'b': 'b', 'a': 'a'}, 'a': 'zeta'},
        dict.fromkeys((f'key{index}' for index in range(40)), 'same'),
        'root string',
        12345,
    ]


def _table_widths() -> list[bytes]:
    """Object counts and offset-table offsets on each side of the 1-, 2- and 4-byte widths."""
    sources = [_binary(list(range(count - 1))) for count in (255, 256, 257, 65535, 65536, 65537)]
    for target in (255, 256, 65535, 65536):
        for length in range(target - 20, target):
            source = _binary([b'x' * length])
            if struct.unpack('>Q', source[-8:])[0] == target:
                sources.append(source)
                break
        else:
            raise ValueError(f'no data length puts the offset table at {target}')
    sources.append(bplist([b'\xa1\x00\x01', b'\x10\x07'], ref_size=2, offset_size=2))
    return sources


def _mutants(sources: list[bytes], per_source: int) -> list[bytes]:
    rng = random.Random(20261126)
    mutants = []
    for source in sources:
        for _ in range(per_source):
            data = bytearray(source)
            for _ in range(rng.randint(1, 3)):
                data[rng.randrange(len(data))] = rng.randrange(256)
            mutants.append(bytes(data))
        mutants.extend(source[:cut] for cut in rng.sample(range(len(source)), min(5, len(source))))
    return mutants


def round_trip_sources() -> list[bytes]:
    """Every round-trip source: plistlib's own, hand-made ones, and seeded mutations of the small ones."""
    generated = [_binary(value) for value in _numbers() + _texts() + _containers()]
    handmade = handmade_sources()
    small = [source for source in generated + handmade if 32 < len(source) <= 2048]
    return generated + _table_widths() + handmade + _mutants(small, 60)


def _record(kind: int, pieces: list[bytes], expected: bytes | None) -> bytes:
    out = bytes([kind, expected is None])
    for piece in [*pieces, expected or b'']:
        out += struct.pack('>I', len(piece)) + piece
    return out


def main() -> None:
    """Write every case to the file named by the first argument."""
    out = bytearray()
    for source in round_trip_sources():
        out += _record(0, [source], _round_trip(source))
    jobs = [Path(path).read_bytes() for path in sys.argv[2:5]]
    for catalog, job_plists, expected in edit_cases(jobs):
        out += _record(1, [catalog, *job_plists], expected)
    Path(sys.argv[1]).write_bytes(out)


if __name__ == '__main__':
    main()
