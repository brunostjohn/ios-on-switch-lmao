"""Lists a mounted APFS tree through macOS's kernel, one tab-separated line per entry.

    path kind ino uid:gid mode size flags nlink birth mtime ctime atime xattrs content

Every time is the kernel's; birth is `st_birthtime` as Python gives it. `xattrs` is each name with the SHA-256 of its
value, compressed files' decmpfs and resource fork included. `content` is the SHA-256 of the bytes `read` returns
(the kernel decompresses), or a symlink's target. Run as root so every directory can be read; any read error fails.

    sudo python3 tree_list.py <directory>
"""

import ctypes
import hashlib
import os
import stat
import sys
from typing import TextIO

XATTR_NOFOLLOW = 0x0001
XATTR_SHOWCOMPRESSION = 0x0020
OPTIONS = XATTR_NOFOLLOW | XATTR_SHOWCOMPRESSION
ENOATTR = 93
# listxattr never names the ACL, but root can read it by name.
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


def xattr_names(raw: bytes, path: str) -> list[str]:
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


def xattr(raw: bytes, name: str, path: str) -> bytes:
    key = name.encode()
    size = check(libc.getxattr(raw, key, None, 0, 0, OPTIONS), path)
    buf = ctypes.create_string_buffer(max(size, 1))
    got = check(libc.getxattr(raw, key, buf, size, 0, OPTIONS), path)
    if got != size:
        raise OSError(f'{path}: {name} read {got} of {size} bytes')
    return buf.raw[:size]


def escape(s: str) -> str:
    return s.replace('\\', '\\\\').replace('\t', '\\t').replace('\n', '\\n')


def kind(mode: int) -> str:
    if stat.S_ISDIR(mode):
        return 'd'
    if stat.S_ISLNK(mode):
        return 'l'
    return 'f' if stat.S_ISREG(mode) else 'o'


def content(full: str, st: os.stat_result) -> str:
    if stat.S_ISLNK(st.st_mode):
        return escape(os.readlink(full))
    if not stat.S_ISREG(st.st_mode):
        return '-'
    h = hashlib.sha256()
    with open(full, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def line(full: str, rel: str) -> str:
    st = os.lstat(full)
    raw = os.fsencode(full)
    names = xattr_names(raw, full)
    xattrs = ','.join(f'{escape(n)}={hashlib.sha256(xattr(raw, n, full)).hexdigest()[:16]}' for n in names)
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
            repr(st.st_birthtime),
            str(st.st_mtime_ns),
            str(st.st_ctime_ns),
            str(st.st_atime_ns),
            xattrs or '-',
            content(full, st),
        ]
    )


def walk(root: str, out: TextIO) -> None:
    pending = [(root, '/')]
    while pending:
        full, rel = pending.pop()
        out.write(line(full, rel) + '\n')
        if os.path.isdir(full) and not os.path.islink(full):
            pending.extend((e.path, rel.rstrip('/') + '/' + e.name) for e in os.scandir(full))


def main() -> None:
    walk(sys.argv[1], sys.stdout)


if __name__ == '__main__':
    main()
