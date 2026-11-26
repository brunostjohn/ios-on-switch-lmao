"""Binary plists plistlib never writes.

CoreFoundation's object order, shared and unreferenced objects, wide integers, odd size and reference widths, and
broken files.
"""

import struct

STR_A = b'\x51a'
STR_B = b'\x51b'
INT_7 = b'\x10\x07'
TRUE = b'\x09'


def bplist(  # noqa: PLR0913  each keyword is one trailer or table field a case bends
    objects: list[bytes],
    top: int = 0,
    *,
    ref_size: int = 1,
    offset_size: int | None = None,
    alias: dict[int, int] | None = None,
    place: dict[int, int] | None = None,
    pad: int = 0,
    trailer: dict[str, int] | None = None,
) -> bytes:
    """Lay `objects` out after the header and `pad` zero bytes.

    `alias` gives an entry another entry's offset and `place` an absolute one; neither writes the entry's bytes.
    """
    alias, place = alias or {}, place or {}
    body = bytearray(b'bplist00') + bytes(pad)
    offsets = [0] * len(objects)
    for index, data in enumerate(objects):
        if index not in alias and index not in place:
            offsets[index] = len(body)
            body += data
    for index, target in alias.items():
        offsets[index] = offsets[target]
    for index, offset in place.items():
        offsets[index] = offset
    table = len(body)
    size = offset_size if offset_size is not None else (1 if table < 256 else 2 if table < 65536 else 4)
    for offset in offsets:
        body += offset.to_bytes(size, 'big')
    fields = {'offset_size': size, 'ref_size': ref_size, 'count': len(objects), 'top': top, 'table': table}
    fields |= trailer or {}
    body += bytes(6) + struct.pack('>BBQQQ', *fields.values())
    return bytes(body)


def refs(*numbers: int, size: int = 1) -> bytes:
    return b''.join(number.to_bytes(size, 'big') for number in numbers)


def _marker(token: int, count: int) -> bytes:
    return bytes([token | count]) if count < 15 else bytes([token | 0xF, 0x10, count])


def array(*numbers: int, size: int = 1) -> bytes:
    return _marker(0xA0, len(numbers)) + refs(*numbers, size=size)


def dictionary(keys: list[int], values: list[int], size: int = 1) -> bytes:
    return _marker(0xD0, len(keys)) + refs(*keys, size=size) + refs(*values, size=size)


def utf16(text: str) -> bytes:
    data = text.encode('utf-16be')
    return bytes([0x60 | len(data) // 2]) + data


def real32(hex_bits: str) -> bytes:
    return b'\x22' + bytes.fromhex(hex_bits)


def date(seconds: float) -> bytes:
    return b'\x33' + struct.pack('>d', seconds)


def _layouts() -> list[bytes]:
    return [
        bplist([INT_7, STR_B, STR_A, dictionary([2, 1], [0, 0])], top=3),
        bplist([array(1, 1), dictionary([2], [3]), STR_A, INT_7]),
        bplist([array(1, 2), array(3), array(3), INT_7], alias={2: 1}),
        bplist([array(1), INT_7, b'\x70', b'\xff\xff', STR_A]),
        bplist([dictionary([1, 2, 3], [4, 5, 6]), STR_A, STR_B, utf16('a'), INT_7, TRUE, b'\x10\x03']),
        bplist([array(0)]),
        bplist([dictionary([1], [0]), STR_A]),
        bplist([array(1, 2), array(2, 0), array(1)]),
        bplist([INT_7]),
        bplist([b'\x00']),
        bplist([array(1, 2), b'\x00', b'\x00']),
        bplist([array(1)], trailer={'count': 2}),
    ]


def _integers() -> list[bytes]:
    wide = [
        b'\x13' + (5).to_bytes(8, 'big'),
        b'\x12\x00\x00\x00\x01',
        b'\x14' + (5).to_bytes(16, 'big'),
        b'\x14' + (-5).to_bytes(16, 'big', signed=True),
        b'\x14' + (2**64 - 1).to_bytes(16, 'big'),
        b'\x15' + (77).to_bytes(32, 'big'),
        b'\x15' + (-(2**63)).to_bytes(32, 'big', signed=True),
        b'\x11\x00\x07',
        b'\x10\x00',
        b'\x09',
    ]
    huge = b'\x15' + (2**200).to_bytes(32, 'big')
    return [
        bplist([array(*range(1, len(wide) + 1)), *wide]),
        bplist([b'\x14' + (2**64).to_bytes(16, 'big')]),
        bplist([b'\x14' + (-(2**63) - 1).to_bytes(16, 'big', signed=True)]),
        bplist([b'\x15' + (-(2**100)).to_bytes(32, 'big', signed=True)]),
        bplist([huge]),
        bplist([dictionary([1, 1], [2, 3]), STR_A, huge, INT_7]),
    ]


def _eof_integer() -> bytes:
    """Return a plist whose integer token is its last byte, the table offset's low, so `fp.read` finds no bytes."""
    objects = [array(1, 2), INT_7, b'']
    table = 0x13
    pad = table - 8 - len(objects[0]) - len(objects[1])
    return bplist(objects, pad=pad, place={2: table + len(objects) + 32 - 1})


def _reals_and_dates() -> list[bytes]:
    reals = [real32(bits) for bits in ('3fc00000', '80000000', '7f800001', 'ffa00000', '7fc00001', '7f800000')]
    reals += [real32('00000001'), b'\x23' + bytes(8), b'\x23\x80' + bytes(7), b'\x23\x7f\xf8' + bytes(5) + b'\x01']
    seconds = [0.0, -0.0, 5e-7, -5e-7, 1.5e-6, 2.5e-6, -2.5e-6, 1e-7, 123456789.123456789, -6.3113904e10]
    seconds += [-63113904000.0, 2.5e11, 252423993599.99997, 1e-300, 0.1, 2e11 + 0.5e-6, 2e11 + 1.5e-6]
    dates = [date(value) for value in seconds]
    return [
        bplist([array(*range(1, len(reals) + 1)), *reals]),
        bplist([array(*range(1, len(dates) + 1)), *dates]),
        *[bplist([date(value)]) for value in (1e12, -1e12, float('nan'), float('inf'), 2.524239936e11 + 86400)],
        bplist([array(1, 1, 2), real32('7fc00000'), real32('7fc00000')]),
        bplist([array(1, 1, 2), b'\x23\x7f\xf8' + bytes(6), b'\x23\x7f\xf8' + bytes(6)]),
        bplist([array(1, 2), b'\x23\x7f\xf8' + bytes(6), b'\x23\x7f\xf8' + bytes(6)], alias={2: 1}),
    ]


def _sizes_and_strings() -> list[bytes]:
    items = [
        b'\x0f',
        b'\x40',
        utf16('abc'),
        b'\x53abc',
        b'\x5f\x10\x03abc',
        b'\x5f\x30\x03abc',
        b'\x5f\x11\x00\x03abc',
        b'\x4f\x13' + (2).to_bytes(8, 'big') + b'hi',
        utf16('é😀'),
        b'\x80\x05',
        b'\x82\x00\x01\x00',
        b'\x8f' + (9).to_bytes(16, 'big'),
    ]
    uid = b'\x81\x01\x00'
    return [
        bplist([array(*range(1, len(items) + 1)), *items]),
        bplist([array(1, 1, 2, 3), uid, uid, uid], alias={3: 2}),
        bplist([b'\x8f' + (2**64).to_bytes(16, 'big')]),
        bplist([b'\x53ab\x80']),
        bplist([b'\x61\xd8\x00']),
        bplist([b'\x5f\x13' + (2**40).to_bytes(8, 'big')]),
        bplist([b'\x45abc']),
    ]


def _widths() -> list[bytes]:
    objects = [array(1, 2, size=3), STR_A, dictionary([1], [3], size=3), INT_7]
    return [
        bplist(objects, ref_size=3, offset_size=3),
        bplist([array(1, 2, size=7), STR_A, dictionary([1], [3], size=7), INT_7], ref_size=7, offset_size=5),
        bplist([INT_7], ref_size=0),
        bplist([b'\xa0'], ref_size=0),
        bplist([INT_7], trailer={'offset_size': 0}),
        bplist([array(1, size=16), INT_7], ref_size=16, offset_size=16),
        bplist([array(1), INT_7], trailer={'offset_size': 9}),
    ]


def _broken() -> list[bytes]:
    bad_tokens = [b'\x01', b'\x0c', b'\x21', b'\x24', b'\x30', b'\x70', b'\x90', b'\xb0', b'\xc0', b'\xe0', b'\xf0']
    return [
        b'',
        b'bplist00' + bytes(20),
        bplist([INT_7], trailer={'count': 0}),
        bplist([INT_7], trailer={'top': 1}),
        bplist([array(5), INT_7]),
        bplist([INT_7], trailer={'table': 2**63}),
        bplist([INT_7], trailer={'table': 2**63 - 1}),
        bplist([INT_7], trailer={'table': 4000}),
        bplist([dictionary([1], [2]), array(), INT_7]),
        bplist([dictionary([1], [2]), INT_7, INT_7]),
        bplist([dictionary([1], [2]), b'\x00', INT_7]),
        bplist([array(1), INT_7])[:-1],
        *[bplist([token]) for token in bad_tokens],
        b'nonsense' + bplist([INT_7])[8:],
    ]


def handmade_sources() -> list[bytes]:
    """Every hand-assembled source."""
    return [
        *_layouts(),
        *_integers(),
        _eof_integer(),
        *_reals_and_dates(),
        *_sizes_and_strings(),
        *_widths(),
        *_broken(),
    ]
