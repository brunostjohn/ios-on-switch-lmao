"""Dump a mounted APFS volume to canonical JSON and deep-diff two dumps."""

from __future__ import annotations

import argparse
import collections
import ctypes
import dataclasses
import errno
import hashlib
import json
import os
import stat
import sys
from pathlib import Path
from typing import Any

XATTR_NOFOLLOW = 0x0001
XATTR_SHOWCOMPRESSION = 0x0020
DARWIN_XATTR_OPTS = XATTR_NOFOLLOW | XATTR_SHOWCOMPRESSION
ENOATTR = 93
ACL_TYPE_EXTENDED = 0x00000100
UNLISTED_DARWIN_XATTRS = ('com.apple.system.Security',)

BSD_FLAG_NAMES: tuple[tuple[int, str], ...] = (
    (0x00000002, 'UF_IMMUTABLE'),
    (0x00000020, 'UF_COMPRESSED'),
    (0x00008000, 'UF_HIDDEN'),
    (0x00020000, 'SF_IMMUTABLE'),
)


@dataclasses.dataclass(frozen=True)
class DiffOptions:
    compare_mtime: bool = False
    ignore_root: bool = False
    ignore_dir_size: bool = False
    ignore_paths: tuple[str, ...] = ()
    ignore_xattrs: tuple[str, ...] = ()
    max_diffs: int = 50


def _darwin_libc() -> ctypes.CDLL | None:
    if sys.platform != 'darwin':
        return None
    lib = ctypes.CDLL(None, use_errno=True)
    lib.listxattr.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_int]
    lib.listxattr.restype = ctypes.c_ssize_t
    lib.getxattr.argtypes = [
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_void_p,
        ctypes.c_size_t,
        ctypes.c_uint32,
        ctypes.c_int,
    ]
    lib.getxattr.restype = ctypes.c_ssize_t
    lib.acl_get_link_np.argtypes = [ctypes.c_char_p, ctypes.c_int]
    lib.acl_get_link_np.restype = ctypes.c_void_p
    lib.acl_to_text.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ssize_t)]
    lib.acl_to_text.restype = ctypes.c_void_p
    lib.acl_free.argtypes = [ctypes.c_void_p]
    lib.acl_free.restype = ctypes.c_int
    return lib


_LIBC = _darwin_libc()


def _check_darwin(ret: int, path: str) -> int:
    if ret < 0:
        err = ctypes.get_errno()
        raise OSError(err, os.strerror(err), path)
    return ret


def _read_xattrs_darwin(path: str) -> dict[str, dict[str, Any]]:
    assert _LIBC is not None
    raw = os.fsencode(path)
    size = _check_darwin(_LIBC.listxattr(raw, None, 0, DARWIN_XATTR_OPTS), path)
    buf = ctypes.create_string_buffer(max(size, 1))
    got = _check_darwin(_LIBC.listxattr(raw, buf, size, DARWIN_XATTR_OPTS), path)
    names = [n.decode('utf-8', 'surrogateescape') for n in buf.raw[:got].split(b'\0') if n]
    if os.geteuid() == 0:
        for extra in UNLISTED_DARWIN_XATTRS:
            if extra in names:
                continue
            ctypes.set_errno(0)
            if _LIBC.getxattr(raw, extra.encode(), None, 0, 0, DARWIN_XATTR_OPTS) >= 0:
                names.append(extra)
            elif ctypes.get_errno() != ENOATTR:
                _check_darwin(-1, path)

    out: dict[str, dict[str, Any]] = {}
    for name in sorted(set(names)):
        key = name.encode('utf-8', 'surrogateescape')
        xsize = _check_darwin(_LIBC.getxattr(raw, key, None, 0, 0, DARWIN_XATTR_OPTS), path)
        xbuf = ctypes.create_string_buffer(max(xsize, 1))
        xgot = _check_darwin(_LIBC.getxattr(raw, key, xbuf, xsize, 0, DARWIN_XATTR_OPTS), path)
        if xgot != xsize:
            raise OSError(f'{path}: xattr {name} read {xgot} of {xsize} bytes')
        val = xbuf.raw[:xsize]
        out[name] = {'size': xsize, 'sha256': hashlib.sha256(val).hexdigest()}
    return out


def _read_xattrs_posix(path: str) -> dict[str, dict[str, Any]]:
    if not hasattr(os, 'listxattr'):
        return {}
    try:
        names = sorted(os.listxattr(path, follow_symlinks=False))
    except OSError as exc:
        if exc.errno in (errno.ENOTSUP, errno.EOPNOTSUPP):
            return {}
        raise
    out: dict[str, dict[str, Any]] = {}
    for name in names:
        val = os.getxattr(path, name, follow_symlinks=False)
        out[name] = {'size': len(val), 'sha256': hashlib.sha256(val).hexdigest()}
    return out


def read_xattrs(path: str) -> dict[str, dict[str, Any]]:
    if _LIBC is not None:
        return _read_xattrs_darwin(path)
    return _read_xattrs_posix(path)


def read_acl(path: str) -> list[str]:
    if _LIBC is None:
        return []
    raw = os.fsencode(path)
    ctypes.set_errno(0)
    acl = _LIBC.acl_get_link_np(raw, ACL_TYPE_EXTENDED)
    if not acl:
        err = ctypes.get_errno()
        if err in (0, errno.ENOENT, errno.ENOTSUP, errno.EOPNOTSUPP):
            return []
        raise OSError(err, os.strerror(err), path)
    try:
        length = ctypes.c_ssize_t(0)
        text_ptr = _LIBC.acl_to_text(acl, ctypes.byref(length))
        if not text_ptr:
            err = ctypes.get_errno()
            raise OSError(err, os.strerror(err), path)
        try:
            text = ctypes.string_at(text_ptr, length.value).decode('utf-8', 'surrogateescape')
        finally:
            _LIBC.acl_free(text_ptr)
    finally:
        _LIBC.acl_free(acl)
    return [line for line in text.splitlines() if line and not line.startswith('!#acl')]


def _entry_type(mode: int) -> str:
    if stat.S_ISDIR(mode):
        return 'd'
    if stat.S_ISLNK(mode):
        return 'l'
    return 'f' if stat.S_ISREG(mode) else 'o'


def _sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def _dump_entry(full: str) -> dict[str, Any]:
    st = os.lstat(full)
    kind = _entry_type(st.st_mode)
    sha256 = _sha256_file(full) if kind == 'f' else None
    symlink_target = os.readlink(full) if kind == 'l' else None
    return {
        'type': kind,
        'uid': st.st_uid,
        'gid': st.st_gid,
        'mode': f'{stat.S_IMODE(st.st_mode):04o}',
        'flags': getattr(st, 'st_flags', 0),
        'size': st.st_size,
        'sha256': sha256,
        'xattrs': read_xattrs(full),
        'acl': read_acl(full),
        'symlink_target': symlink_target,
        'mtime_ns': st.st_mtime_ns,
        'ino': st.st_ino,
        'nlink': st.st_nlink,
    }


def dump_tree(root: str | Path) -> dict[str, Any]:
    root_str = os.fspath(root)
    if not os.path.isdir(root_str):
        raise OSError(errno.ENOTDIR, 'not a directory', root_str)

    entries: dict[str, dict[str, Any]] = {}
    by_ino: dict[int, list[str]] = collections.defaultdict(list)
    stack: list[tuple[str, str]] = [(root_str, '/')]

    while stack:
        full, rel = stack.pop()
        entry = _dump_entry(full)
        entries[rel] = entry
        if entry['type'] != 'd':
            by_ino[entry['ino']].append(rel)
        if entry['type'] == 'd':
            base = '' if rel == '/' else rel
            with os.scandir(full) as it:
                children = sorted((e.name, e.path) for e in it)
            for name, child_path in reversed(children):
                stack.append((child_path, f'{base}/{name}'))

    groups = sorted(sorted(paths) for paths in by_ino.values() if len(paths) > 1)
    return {
        'version': 1,
        'source': 'mount',
        'entries': dict(sorted(entries.items())),
        'hardlink_groups': groups,
    }


def format_flags(flags: int) -> str:
    names = [name for bit, name in BSD_FLAG_NAMES if flags & bit]
    if not names:
        return f'{flags:#x}'
    return f'{flags:#x} ({"|".join(names)})'


def _should_ignore(path: str, opts: DiffOptions) -> bool:
    if opts.ignore_root and path == '/':
        return True
    for prefix in opts.ignore_paths:
        norm = prefix.rstrip('/')
        if path == norm or path.startswith(norm + '/'):
            return True
    return False


def _filter_entries(entries: dict[str, dict[str, Any]], opts: DiffOptions) -> dict[str, dict[str, Any]]:
    return {p: e for p, e in entries.items() if not _should_ignore(p, opts)}


def _hardlink_map(entries: dict[str, dict[str, Any]]) -> dict[str, tuple[str, ...]]:
    by_ino: dict[int, list[str]] = collections.defaultdict(list)
    for path, entry in entries.items():
        if entry['type'] != 'd':
            by_ino[entry['ino']].append(path)
    return {path: tuple(sorted(paths)) for paths in by_ino.values() for path in paths}


def _diff_xattrs(
    path: str,
    exp_x: dict[str, dict[str, Any]],
    act_x: dict[str, dict[str, Any]],
    ignore: tuple[str, ...],
) -> list[str]:
    exp_f = {k: v for k, v in exp_x.items() if k not in ignore}
    act_f = {k: v for k, v in act_x.items() if k not in ignore}
    diffs = [f'{path}: missing xattr {name!r}' for name in sorted(set(exp_f) - set(act_f))]
    diffs.extend(f'{path}: unexpected xattr {name!r}' for name in sorted(set(act_f) - set(exp_f)))
    diffs.extend(
        f'{path}: xattr {name!r} differs: expected {exp_f[name]}, got {act_f[name]}'
        for name in sorted(set(exp_f) & set(act_f))
        if exp_f[name] != act_f[name]
    )
    return diffs


def _diff_acl(path: str, exp: dict[str, Any], act: dict[str, Any]) -> list[str]:
    exp_acl = exp.get('acl')
    act_acl = act.get('acl')
    if exp_acl is not None and act_acl is not None:
        if exp_acl != act_acl:
            return [f'{path}: acl differs: expected {exp_acl!r}, got {act_acl!r}']
        return []
    # Offline plan dumps leave acl=None and verify the raw Security xattr bytes; cross-check presence here.
    if exp_acl is None and act_acl is not None:
        has_sec = 'com.apple.system.Security' in exp['xattrs']
        if bool(act_acl) != has_sec:
            return [f'{path}: acl presence mismatch: plan Security xattr={has_sec}, mount acl={act_acl!r}']
    elif act_acl is None and exp_acl is not None:
        has_sec = 'com.apple.system.Security' in act['xattrs']
        if bool(exp_acl) != has_sec:
            return [f'{path}: acl presence mismatch: expected acl={exp_acl!r}, actual Security xattr={has_sec}']
    return []


def _diff_entry(path: str, exp: dict[str, Any], act: dict[str, Any], opts: DiffOptions) -> list[str]:
    diffs = [
        f'{path}: {field} differs: expected {exp[field]!r}, got {act[field]!r}'
        for field in ('type', 'uid', 'gid', 'mode')
        if exp[field] != act[field]
    ]
    if exp['flags'] != act['flags']:
        diffs.append(f'{path}: flags differ: expected {format_flags(exp["flags"])}, got {format_flags(act["flags"])}')
    if (exp['type'] != 'd' or not opts.ignore_dir_size) and exp['size'] != act['size']:
        diffs.append(f'{path}: size differs: expected {exp["size"]}, got {act["size"]}')
    if exp['sha256'] != act['sha256']:
        diffs.append(f'{path}: sha256 differs: expected {exp["sha256"]}, got {act["sha256"]}')
    if exp['symlink_target'] != act['symlink_target']:
        diffs.append(
            f'{path}: symlink_target differs: expected {exp["symlink_target"]!r}, got {act["symlink_target"]!r}'
        )
    if opts.compare_mtime and exp['mtime_ns'] != act['mtime_ns']:
        diffs.append(f'{path}: mtime_ns differs: expected {exp["mtime_ns"]}, got {act["mtime_ns"]}')
    diffs.extend(_diff_xattrs(path, exp['xattrs'], act['xattrs'], opts.ignore_xattrs))
    diffs.extend(_diff_acl(path, exp, act))
    return diffs


def diff_dumps(expected: dict[str, Any], actual: dict[str, Any], opts: DiffOptions | None = None) -> list[str]:
    cfg = opts or DiffOptions()
    exp_entries = _filter_entries(expected['entries'], cfg)
    act_entries = _filter_entries(actual['entries'], cfg)
    diffs = [f'{path}: missing in actual' for path in sorted(set(exp_entries) - set(act_entries))]
    diffs.extend(f'{path}: unexpected in actual' for path in sorted(set(act_entries) - set(exp_entries)))

    exp_hl = _hardlink_map(exp_entries)
    act_hl = _hardlink_map(act_entries)
    for path in sorted(set(exp_entries) & set(act_entries)):
        diffs.extend(_diff_entry(path, exp_entries[path], act_entries[path], cfg))
        if exp_entries[path]['type'] != 'd' and act_entries[path]['type'] != 'd' and exp_hl[path] != act_hl[path]:
            diffs.append(f'{path}: hardlink group differs: expected {exp_hl[path]}, got {act_hl[path]}')
    return diffs


def _cmd_dump(args: argparse.Namespace) -> int:
    data = dump_tree(args.root)
    encoded = json.dumps(data, indent=2, sort_keys=True) + '\n'
    if args.out:
        Path(args.out).write_text(encoded, encoding='utf-8')
    else:
        sys.stdout.write(encoded)
    return 0


def _cmd_diff(args: argparse.Namespace) -> int:
    expected = json.loads(Path(args.expected).read_text(encoding='utf-8'))
    actual = json.loads(Path(args.actual).read_text(encoding='utf-8'))
    opts = DiffOptions(
        compare_mtime=args.compare_mtime,
        ignore_root=args.ignore_root,
        ignore_dir_size=args.ignore_dir_size,
        ignore_paths=tuple(args.ignore_path),
        ignore_xattrs=tuple(args.ignore_xattr),
        max_diffs=args.max_diffs,
    )
    diffs = diff_dumps(expected, actual, opts)
    exp_count = len(_filter_entries(expected['entries'], opts))
    act_count = len(_filter_entries(actual['entries'], opts))
    if diffs:
        print(f'DIFF FAILED: {len(diffs)} difference(s) across {exp_count} expected / {act_count} actual entries')
        for line in diffs[: opts.max_diffs]:
            print(f'  {line}')
        if len(diffs) > opts.max_diffs:
            print(f'  ... and {len(diffs) - opts.max_diffs} more')
        return 1
    print(f'OK: {act_count} entries match')
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description='Dump and deep-diff APFS trees.')
    sub = parser.add_subparsers(dest='cmd', required=True)

    p_dump = sub.add_parser('dump')
    p_dump.add_argument('root')
    p_dump.add_argument('--out')
    p_dump.set_defaults(func=_cmd_dump)

    p_diff = sub.add_parser('diff')
    p_diff.add_argument('expected')
    p_diff.add_argument('actual')
    p_diff.add_argument('--compare-mtime', action='store_true')
    p_diff.add_argument('--ignore-root', action='store_true')
    p_diff.add_argument('--ignore-dir-size', action='store_true')
    p_diff.add_argument('--ignore-path', action='append', default=[])
    p_diff.add_argument('--ignore-xattr', action='append', default=[])
    p_diff.add_argument('--max-diffs', type=int, default=50)
    p_diff.set_defaults(func=_cmd_diff)

    args = parser.parse_args(argv)
    return int(args.func(args))


if __name__ == '__main__':
    raise SystemExit(main())
