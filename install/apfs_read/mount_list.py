"""Lists a mounted APFS volume through macOS's kernel, in the format `switch_apfs_read_tool walk` prints.

    sudo python3 mount_list.py <mount point> [hash_every]

Run as root so every directory can be read. Any read error fails the listing.
"""

import ctypes
import hashlib
import os
import stat
import sys
from typing import TextIO

XATTR_NOFOLLOW = 0x0001
XATTR_SHOWCOMPRESSION = 0x0020
UF_COMPRESSED = 0x20
OPTIONS = XATTR_NOFOLLOW | XATTR_SHOWCOMPRESSION
ENOATTR = 93
# listxattr never names these, even with XATTR_SHOWCOMPRESSION, but root can read them.
UNLISTED = ['com.apple.system.Security']

libc = ctypes.CDLL(None, use_errno=True)
libc.listxattr.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_int]
libc.listxattr.restype = ctypes.c_ssize_t
libc.getxattr.argtypes = [
    ctypes.c_char_p,
    ctypes.c_char_p,
    ctypes.c_void_p,
    ctypes.c_size_t,
    ctypes.c_uint32,
    ctypes.c_int,
]
libc.getxattr.restype = ctypes.c_ssize_t


def check(n: int, path: str) -> int:
    if n < 0:
        err = ctypes.get_errno()
        raise OSError(err, os.strerror(err), path)
    return n


def xattr_names(path: str) -> list[str]:
    raw = os.fsencode(path)
    size = check(libc.listxattr(raw, None, 0, OPTIONS), path)
    buf = ctypes.create_string_buffer(size)
    size = check(libc.listxattr(raw, buf, size, OPTIONS), path)
    names = [n.decode() for n in buf.raw[:size].split(b'\0') if n]
    for name in UNLISTED:
        if libc.getxattr(raw, name.encode(), None, 0, 0, OPTIONS) >= 0:
            names.append(name)
        elif ctypes.get_errno() != ENOATTR:
            check(-1, path)
    return sorted(names)


def xattr(path: str, name: str) -> bytes:
    raw, key = os.fsencode(path), name.encode()
    size = check(libc.getxattr(raw, key, None, 0, 0, OPTIONS), path)
    buf = ctypes.create_string_buffer(size)
    got = check(libc.getxattr(raw, key, buf, size, 0, OPTIONS), path)
    if got != size:
        raise OSError(f'{path}: {name} read {got} of {size} bytes')
    return buf.raw[:size]


def escape(s: str) -> str:
    return s.replace('\\', '\\\\').replace('\t', '\\t').replace('\n', '\\n')


def sampled(path: str, every: int) -> bool:
    return every != 0 and hashlib.sha256(path.encode()).digest()[0] % every == 0


def content(full: str, rel: str, st: os.stat_result, names: list[str], every: int) -> str:
    if stat.S_ISDIR(st.st_mode):
        return '-'
    if stat.S_ISLNK(st.st_mode):
        return escape(os.readlink(full))
    if not sampled(rel, every):
        return '-'
    if st.st_flags & UF_COMPRESSED:
        decmpfs = xattr(full, 'com.apple.decmpfs')
        fork = xattr(full, 'com.apple.ResourceFork') if 'com.apple.ResourceFork' in names else b''
        kind = int.from_bytes(decmpfs[4:8], 'little')
        return f'c{kind}:{hashlib.sha256(decmpfs).hexdigest()}:{hashlib.sha256(fork).hexdigest()}'
    h = hashlib.sha256()
    with open(full, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def kind(mode: int) -> str:
    if stat.S_ISDIR(mode):
        return 'd'
    if stat.S_ISLNK(mode):
        return 'l'
    return 'f' if stat.S_ISREG(mode) else 'o'


def line(full: str, rel: str, every: int) -> str:
    st = os.lstat(full)
    names = xattr_names(full)
    return '\t'.join(
        [
            escape(rel),
            kind(st.st_mode),
            str(st.st_ino),
            f'{st.st_uid}:{st.st_gid}',
            f'{stat.S_IMODE(st.st_mode):o}',
            str(st.st_size),
            f'{st.st_flags:#x}',
            str(st.st_nlink),
            str(st.st_mtime_ns),
            escape(','.join(names)) if names else '-',
            content(full, rel, st, names, every),
        ]
    )


def walk(root: str, every: int, out: TextIO) -> None:
    pending = [(root, '/')]
    while pending:
        full, rel = pending.pop()
        out.write(line(full, rel, every) + '\n')
        if os.path.isdir(full) and not os.path.islink(full):
            for e in os.scandir(full):
                child = rel.rstrip('/') + '/' + e.name
                pending.append((e.path, child))


def main() -> None:
    every = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    walk(sys.argv[1], every, sys.stdout)


if __name__ == '__main__':
    main()
