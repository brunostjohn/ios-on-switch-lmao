#!/usr/bin/env python3
"""LRU-prunes a shared Bazel disk cache before saving it to CI cache storage.

Bazel refreshes a disk-cache entry's mtime when writing or reading it, but a cache hit on a downstream
artifact never reads untouched intermediate blobs, so deleting everything older than the run start wipes out
warm entries needed by the next edit or by other jobs sharing the cache. Instead, keep all entries touched
during this run and evict only expired or oldest untouched entries once total size exceeds max_gib.
"""

import argparse
import contextlib
import os
import sys
import time
from pathlib import Path


def _scan_files(disk: Path) -> list[tuple[int, int, Path]]:
    entries: list[tuple[int, int, Path]] = []
    for root, _dirs, files in os.walk(disk):
        root_path = Path(root)
        for name in files:
            path = root_path / name
            try:
                st = path.lstat()
            except OSError:
                continue
            if not path.is_symlink():
                entries.append((st.st_mtime_ns, st.st_size, path))
    return entries


def _remove_file(path: Path) -> bool:
    try:
        path.unlink()
    except OSError:
        return False
    return True


def _remove_empty_dirs(disk: Path) -> None:
    for root, dirs, files in os.walk(disk, topdown=False):
        if dirs or files:
            continue
        root_path = Path(root)
        if root_path == disk:
            continue
        with contextlib.suppress(OSError):
            root_path.rmdir()


def _drop_expired(
    entries: list[tuple[int, int, Path]],
    cutoff_ns: int,
    marker_ns: int,
) -> tuple[list[tuple[int, int, Path]], int, int]:
    kept: list[tuple[int, int, Path]] = []
    evicted_count = 0
    evicted_bytes = 0
    for mtime_ns, size, path in entries:
        if mtime_ns < marker_ns and mtime_ns < cutoff_ns and _remove_file(path):
            evicted_count += 1
            evicted_bytes += size
        else:
            kept.append((mtime_ns, size, path))
    return kept, evicted_count, evicted_bytes


def _drop_lru_to_target(
    entries: list[tuple[int, int, Path]],
    disk: Path,
    marker_ns: int,
    target_bytes: int,
) -> tuple[int, int, int, int]:
    total_bytes = sum(size for _, size, _ in entries)
    untouched = sorted((e for e in entries if e[0] < marker_ns), key=lambda e: (e[0], str(e[2])))
    evicted_count = 0
    evicted_bytes = 0
    max_evicted_cas_ns: int | None = None
    evicted_paths: set[Path] = set()
    cas_dir = disk / 'cas'
    ac_dir = disk / 'ac'

    for mtime_ns, size, path in untouched:
        if total_bytes <= target_bytes:
            break
        if _remove_file(path):
            total_bytes -= size
            evicted_count += 1
            evicted_bytes += size
            evicted_paths.add(path)
            if cas_dir in path.parents:
                max_evicted_cas_ns = mtime_ns if max_evicted_cas_ns is None else max(max_evicted_cas_ns, mtime_ns)

    # Drop AC entries at or older than the newest evicted CAS blob so retained action results stay consistent.
    if max_evicted_cas_ns is not None:
        for mtime_ns, size, path in untouched:
            if path in evicted_paths or ac_dir not in path.parents:
                continue
            if mtime_ns <= max_evicted_cas_ns and _remove_file(path):
                total_bytes -= size
                evicted_count += 1
                evicted_bytes += size

    kept_count = len(entries) - evicted_count
    return kept_count, total_bytes, evicted_count, evicted_bytes


def prune_disk_cache(
    disk: Path,
    marker_ns: int,
    max_bytes: int,
    target_bytes: int,
    max_age_days: float,
) -> dict[str, int | bool]:
    if not disk.is_dir():
        return {'save': False, 'new_files': 0, 'kept_files': 0, 'kept_bytes': 0, 'evicted_files': 0, 'evicted_bytes': 0}

    entries = _scan_files(disk)
    new_files = sum(1 for mtime_ns, _, _ in entries if mtime_ns > marker_ns)
    if new_files == 0:
        return {
            'save': False,
            'new_files': 0,
            'kept_files': len(entries),
            'kept_bytes': sum(size for _, size, _ in entries),
            'evicted_files': 0,
            'evicted_bytes': 0,
        }

    cutoff_ns = time.time_ns() - int(max_age_days * 86400 * 1_000_000_000)
    entries, evicted_count, evicted_bytes = _drop_expired(entries, cutoff_ns, marker_ns)
    total_bytes = sum(size for _, size, _ in entries)
    if total_bytes > max_bytes:
        kept_files, kept_bytes, lru_count, lru_bytes = _drop_lru_to_target(entries, disk, marker_ns, target_bytes)
        evicted_count += lru_count
        evicted_bytes += lru_bytes
    else:
        kept_files = len(entries)
        kept_bytes = total_bytes

    if evicted_count > 0:
        _remove_empty_dirs(disk)

    return {
        'save': True,
        'new_files': new_files,
        'kept_files': kept_files,
        'kept_bytes': kept_bytes,
        'evicted_files': evicted_count,
        'evicted_bytes': evicted_bytes,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--disk', type=Path, required=True)
    parser.add_argument('--marker', type=Path, required=True)
    parser.add_argument('--max-gib', type=float, default=10.0)
    parser.add_argument('--target-gib', type=float, default=8.5)
    parser.add_argument('--max-age-days', type=float, default=14.0)
    parser.add_argument('--github-output', type=Path)
    args = parser.parse_args()

    if not args.marker.is_file():
        if args.github_output:
            with args.github_output.open('a', encoding='utf-8') as out:
                out.write('save=false\n')
        print('prune_disk_cache: marker missing; skipping cache save', file=sys.stderr)
        return

    gib = 1024 * 1024 * 1024
    result = prune_disk_cache(
        disk=args.disk,
        marker_ns=args.marker.stat().st_mtime_ns,
        max_bytes=int(args.max_gib * gib),
        target_bytes=int(args.target_gib * gib),
        max_age_days=args.max_age_days,
    )
    mib = 1024 * 1024
    print(
        f'prune_disk_cache: save={result["save"]} new={result["new_files"]} '
        f'kept={result["kept_files"]} ({int(result["kept_bytes"]) / mib:.1f} MiB) '
        f'evicted={result["evicted_files"]} ({int(result["evicted_bytes"]) / mib:.1f} MiB)',
        file=sys.stderr,
    )
    if args.github_output:
        with args.github_output.open('a', encoding='utf-8') as out:
            out.write(f'save={"true" if result["save"] else "false"}\n')


if __name__ == '__main__':
    main()
