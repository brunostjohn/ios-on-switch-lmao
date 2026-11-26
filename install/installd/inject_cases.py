"""installd.py inject on the OS volume's installd and on universal and damaged variants of it: switch_installd's oracle.

    inject_cases.py <installd> <out>

Each case is the input (zlib) and what inject made of it: the SHA-256 of its output, or the message it refused with.
"""

import hashlib
import struct
import sys
import zlib
from pathlib import Path

from tools.install_policy.installd import LIBRARY, arm64_slice, inject

CPU_ARM64, CPU_X86_64 = 0x100000C, 0x1000007
LC_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB = 0xC, 0x80000018


def universal(slices: list[tuple[int, int, bytes, int]], endian: str = '>', wide: bool = False) -> bytes:
    """Return a universal file of (cpu, subtype, image, align) slices, each at the next multiple of 2**align."""
    entry = struct.Struct(endian + ('IIQQII' if wide else 'IIIII'))
    offset, table = 8 + len(slices) * entry.size, []
    for cpu, subtype, image, align in slices:
        offset = -(-offset // (1 << align)) * (1 << align)
        table.append((cpu, subtype, offset, len(image), align))
        offset += len(image)
    out = bytearray(offset)
    struct.pack_into(endian + 'II', out, 0, 0xCAFEBABF if wide else 0xCAFEBABE, len(slices))
    for index, (cpu, subtype, at, size, align) in enumerate(table):
        fields = (cpu, subtype, at, size, align) + ((0,) if wide else ())
        entry.pack_into(out, 8 + index * entry.size, *fields)
        out[at : at + size] = slices[index][2]
    return bytes(out)


def table_entry(data: bytes, index: int, **fields: int) -> bytes:
    """Return `data`, a big-endian narrow universal file, with fields of one architecture entry replaced."""
    out = bytearray(data)
    at = 8 + index * 20
    values = dict(zip(('cpu', 'subtype', 'offset', 'size', 'align'), struct.unpack_from('>5I', out, at), strict=True))
    struct.pack_into('>5I', out, at, *(values | fields).values())
    return bytes(out)


def word(data: bytes, at: int, value: int) -> bytes:
    out = bytearray(data)
    struct.pack_into('<I', out, at, value)
    return bytes(out)


def commands(image: bytes) -> list[tuple[int, int, int]]:
    count = struct.unpack_from('<I', image, 16)[0]
    found, offset = [], 32
    for _ in range(count):
        cmd, length = struct.unpack_from('<II', image, offset)
        found.append((cmd, offset, length))
        offset += length
    return found


def segment(image: bytes, name: bytes) -> int:
    return next(o for c, o, _ in commands(image) if c == 0x19 and image[o + 8 : o + 24].rstrip(b'\0') == name)


def injected(patched: bytes) -> int:
    """Return the offset of the install policy's command in an injected image."""
    for cmd, offset, _ in commands(patched):
        if cmd == LC_LOAD_DYLIB and LIBRARY.encode() in patched[offset : offset + 64]:
            return offset
    raise ValueError('no injected command')


def second_dependency(patched: bytes) -> bytes:
    """`patched` with a second LC_LOAD_DYLIB of the install policy after its load commands."""
    command = patched[injected(patched) : injected(patched) + 64]
    count, size = struct.unpack_from('<II', patched, 16)
    out = bytearray(patched)
    out[32 + size : 32 + size + 64] = command
    struct.pack_into('<II', out, 16, count + 1, size + 64)
    return bytes(out)


def cases(original: bytes) -> dict[str, bytes]:
    thin = arm64_slice(original)
    subtype = struct.unpack_from('<I', thin, 8)[0]
    patched = inject(thin)
    at = injected(patched)
    size = struct.unpack_from('<I', thin, 20)[0]
    end = 32 + size
    other = bytes(range(256)) * 16
    fat = universal([(CPU_ARM64, subtype, thin, 14)])
    text = segment(thin, b'__TEXT')
    linkedit = segment(thin, b'__LINKEDIT')
    first_dylib = next(o for c, o, _ in commands(thin) if c == LC_LOAD_DYLIB)
    return {
        'original': original,
        'thin': thin,
        'already': patched,
        'universal': fat,
        'universal-x86_64-arm64': universal([(CPU_X86_64, 3, other, 12), (CPU_ARM64, subtype, thin, 14)]),
        'universal-64': universal([(CPU_ARM64, subtype, thin, 14)], wide=True),
        'universal-little-endian': universal([(CPU_ARM64, subtype, thin, 14)], endian='<'),
        'universal-64-little-endian': universal(
            [(CPU_X86_64, 3, other, 12), (CPU_ARM64, subtype, thin, 14)], endian='<', wide=True
        ),
        'universal-already': universal([(CPU_ARM64, subtype, patched, 14)]),
        'universal-two-arm64': universal([(CPU_ARM64, subtype, thin, 14), (CPU_ARM64, subtype, thin, 14)]),
        'universal-empty': struct.pack('>II', 0xCAFEBABE, 0) + bytes(64),
        'universal-truncated': fat[:6],
        'universal-table-past-end': struct.pack('>II', 0xCAFEBABE, 1 << 20) + bytes(64),
        'universal-misaligned': table_entry(fat, 0, align=15),
        'universal-align-64': table_entry(fat, 0, align=64),
        'universal-inside-table': table_entry(fat, 0, offset=8, align=0),
        'universal-past-end': table_entry(fat, 0, size=len(thin) + 1),
        'universal-subtype-differs': table_entry(fat, 0, subtype=subtype ^ 1),
        'universal-small-slice': universal([(CPU_X86_64, 3, other[:16], 12), (CPU_ARM64, subtype, thin, 14)]),
        'universal-overlap': table_entry(
            universal([(CPU_X86_64, 3, other, 12), (CPU_ARM64, subtype, thin, 14)]), 0, offset=1 << 14
        ),
        'universal-bad-slice': universal([(CPU_ARM64, subtype, b'\0' * 4 + thin[4:], 14)]),
        'header-truncated': thin[:20],
        'bad-magic': word(thin, 0, 0xFEEDFACE),
        'x86_64': word(thin, 4, CPU_X86_64),
        'subtype-1': word(thin, 8, 1),
        'arm64e-capabilities': word(thin, 8, 0x80000002),
        'dylib': word(thin, 12, 6),
        'commands-past-end': word(thin, 20, len(thin)),
        'command-size-mismatch': word(thin, 20, size + 8),
        'command-length-odd': word(thin, 36, struct.unpack_from('<I', thin, 36)[0] + 4),
        'no-text': thin[: text + 8] + b'__TEXX' + thin[text + 14 :],
        'segment-past-end': word(thin, linkedit + 48, len(thin) - struct.unpack_from('<Q', thin, linkedit + 40)[0] + 1),
        'padding-dirty': thin[: end + 9] + b'\1' + thin[end + 10 :],
        'padding-short': word(thin, text + 72 + 48, end + 32),
        'weak-policy': word(patched, at, LC_LOAD_WEAK_DYLIB),
        'two-policies': second_dependency(patched),
        'policy-unterminated': patched[: at + 24] + b'A' * 40 + patched[at + 64 :],
        'policy-name-outside': word(patched, at + 8, 64),
        'policy-truncated': word(word(patched, at + 4, 16), 20, size + 64 - 48),
        'dylib-name-not-utf8': thin[: first_dylib + 24] + b'\xff' + thin[first_dylib + 25 :],
    }


def main() -> None:
    source, out = (Path(arg) for arg in sys.argv[1:3])
    records = bytearray()
    for name, data in cases(source.read_bytes()).items():
        try:
            result = b'\1' + hashlib.sha256(inject(data)).digest()
        except (ValueError, struct.error, IndexError) as error:
            message = f'{type(error).__name__}: {error}'.encode()
            result = b'\0' + struct.pack('<H', len(message)) + message
        packed = zlib.compress(data, 6)
        records += struct.pack('<H', len(name)) + name.encode() + struct.pack('<Q', len(packed)) + packed + result
    out.write_bytes(bytes(records))


if __name__ == '__main__':
    main()
