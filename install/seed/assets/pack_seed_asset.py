#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pack IPSW-free shader seed inputs and pre-split pack corpora into a deterministic USTAR tar."""

import argparse
import hashlib
import io
import json
import re
import struct
import tarfile
from pathlib import Path, PurePosixPath
from typing import NamedTuple

MANIFEST_FORMAT = 1
EXPECTED_SPLIT_BUCKETS = 17
MTLB_MAGIC = b'MTLB'
MTLB_HEADER_SIZE = 0x58
HEX_BYTE = re.compile(r'0x([0-9a-fA-F]{2})\b')


class AssetInputs(NamedTuple):
    build: str
    kind: str
    manifest: Path
    qc_sha256: str
    own_metallib: Path
    context_log: Path
    draw_context: Path
    corpus: Path
    archives: Path | None
    embedded: list[Path]
    corpus_report: Path
    split_buckets: list[Path]
    app_libraries: tuple[tuple[str, Path], ...] = ()


def check_rel_path(path: str) -> str:
    parts = PurePosixPath(path).parts
    if not parts or path.startswith('/') or '\0' in path or any(part in ('', '.', '..') for part in parts):
        raise ValueError(f'unsafe relative path: {path!r}')
    return path


def _check_hex(value: str, length: int, label: str) -> str:
    text = value.strip().lower()
    if len(text) != length or any(ch not in '0123456789abcdef' for ch in text):
        raise ValueError(f'invalid {label}: {value!r}')
    return text


def _check_metallib(data: bytes, source: str) -> None:
    if len(data) < MTLB_HEADER_SIZE or data[:4] != MTLB_MAGIC:
        raise ValueError(f'{source}: not a valid MTLB metallib')
    listing_off, listing_size = struct.unpack_from('<QQ', data, 0x18)
    if listing_off + listing_size > len(data) or listing_size < 4:
        raise ValueError(f'{source}: truncated MTLB function listing')
    (count,) = struct.unpack_from('<I', data, listing_off)
    if count == 0:
        raise ValueError(f'{source}: metallib has no functions')


def extract_embedded_header(header: Path) -> tuple[str, bytes]:
    text = header.read_text(encoding='utf-8')
    array = text[text.index('{') : text.rindex('}')]
    data = bytes(int(value, 16) for value in HEX_BYTE.findall(array))
    _check_metallib(data, str(header))
    digest = hashlib.sha256(data).hexdigest()
    return digest, data


def _write_deterministic_tar(entries: dict[str, bytes]) -> bytes:
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode='w', format=tarfile.USTAR_FORMAT) as tar:
        for name in sorted(entries):
            check_rel_path(name)
            raw = entries[name]
            info = tarfile.TarInfo(name)
            info.size = len(raw)
            info.mode = 0o644
            info.mtime = 0
            info.uid = 0
            info.gid = 0
            info.uname = ''
            info.gname = ''
            tar.addfile(info, io.BytesIO(raw))
    return buf.getvalue()


def _collect_app_libraries(
    app_libraries: tuple[tuple[str, Path], ...],
    entries: dict[str, bytes],
) -> list[dict[str, object]]:
    meta: list[dict[str, object]] = []
    seen_guests: set[str] = set()
    for guest_path, lib_path in app_libraries:
        check_rel_path(guest_path)
        if not guest_path.startswith('Applications/') or not guest_path.endswith('.metallib'):
            raise ValueError(f'app library path must be under Applications/*.metallib: {guest_path!r}')
        guest_lower = guest_path.lower()
        if guest_lower in seen_guests:
            raise ValueError(f'duplicate or case-colliding app library guest path: {guest_path!r}')
        seen_guests.add(guest_lower)
        data = lib_path.read_bytes()
        _check_metallib(data, str(lib_path))
        digest = hashlib.sha256(data).hexdigest()
        rel = f'raw/apps/{digest}.metallib'
        if rel in entries and entries[rel] != data:
            raise ValueError(f'conflicting app metallib for {digest}')
        entries[rel] = data
        meta.append({'guest_path': guest_path, 'path': rel, 'sha256': digest, 'size': len(data)})
    meta.sort(key=lambda item: str(item['guest_path']))
    return meta


def _collect_entries(
    inputs: AssetInputs,
) -> tuple[dict[str, bytes], list[int], list[dict[str, object]], list[dict[str, object]]]:
    own_bytes = inputs.own_metallib.read_bytes()
    _check_metallib(own_bytes, str(inputs.own_metallib))
    entries: dict[str, bytes] = {
        'raw/own.metallib': own_bytes,
        'raw/context.log': inputs.context_log.read_bytes(),
        'raw/draw_context.txt': inputs.draw_context.read_bytes(),
        'raw/corpus.log': inputs.corpus.read_bytes(),
        'split/corpus-report.txt': inputs.corpus_report.read_bytes(),
    }
    if inputs.archives is not None:
        entries['raw/archives.tsv'] = inputs.archives.read_bytes()

    embedded_meta: list[dict[str, object]] = []
    for header in inputs.embedded:
        digest, data = extract_embedded_header(header)
        rel = f'raw/embedded/{digest}.metallib'
        if rel in entries and entries[rel] != data:
            raise ValueError(f'conflicting embedded metallib for {digest}')
        if rel not in entries:
            entries[rel] = data
            embedded_meta.append({'path': rel, 'sha256': digest, 'size': len(data)})
    embedded_meta.sort(key=lambda item: str(item['path']))

    app_libraries_meta = _collect_app_libraries(inputs.app_libraries, entries)

    if len(inputs.split_buckets) != EXPECTED_SPLIT_BUCKETS:
        raise ValueError(f'expected {EXPECTED_SPLIT_BUCKETS} split buckets, got {len(inputs.split_buckets)}')
    line_counts: list[int] = []
    for idx, bucket in enumerate(inputs.split_buckets):
        raw = bucket.read_bytes()
        entries[f'split/bucket{idx}.log'] = raw
        line_counts.append(raw.count(b'\n'))
    return entries, line_counts, embedded_meta, app_libraries_meta


def build_tar(inputs: AssetInputs) -> bytes:
    tools_manifest = json.loads(inputs.manifest.read_text(encoding='utf-8'))
    stack_key = _check_hex(str(tools_manifest.get('stack', '')), 16, 'stack key')
    qc_sha256 = _check_hex(inputs.qc_sha256, 64, 'quartzcore_metallib_sha256')
    entries, line_counts, embedded_meta, app_libraries_meta = _collect_entries(inputs)

    inventory = [
        {
            'path': path,
            'sha256': hashlib.sha256(entries[path]).hexdigest(),
            'size': len(entries[path]),
        }
        for path in sorted(entries)
    ]
    manifest = {
        'app_libraries': app_libraries_meta,
        'bucket_counts': {
            'lines': line_counts,
            'split': len(line_counts),
        },
        'build': inputs.build,
        'embedded': embedded_meta,
        'files': inventory,
        'format': MANIFEST_FORMAT,
        'kind': inputs.kind,
        'quartzcore_metallib_sha256': qc_sha256,
        'stack_key': stack_key,
    }
    manifest_bytes = (json.dumps(manifest, indent=2, sort_keys=True) + '\n').encode('utf-8')
    return _write_deterministic_tar({'manifest.json': manifest_bytes, **entries})


def _parse_app_library(spec: str) -> tuple[str, Path]:
    if '=' not in spec:
        raise ValueError(f'--app-library expects GUEST_PATH=FILE, got {spec!r}')
    guest_path, file_path = spec.split('=', 1)
    return guest_path, Path(file_path)


def _parse_args() -> tuple[AssetInputs, Path]:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', required=True)
    parser.add_argument('--kind', required=True)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--qc-sha256', required=True)
    parser.add_argument('--own-metallib', type=Path, required=True)
    parser.add_argument('--context-log', type=Path, required=True)
    parser.add_argument('--draw-context', type=Path, required=True)
    parser.add_argument('--corpus', type=Path, required=True)
    parser.add_argument('--archives', type=Path)
    parser.add_argument('--embedded', type=Path, action='append', default=[])
    parser.add_argument('--app-library', action='append', default=[])
    parser.add_argument('--corpus-report', type=Path, required=True)
    parser.add_argument('--split-bucket', type=Path, nargs='+', required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    inputs = AssetInputs(
        build=args.build,
        kind=args.kind,
        manifest=args.manifest,
        qc_sha256=args.qc_sha256,
        own_metallib=args.own_metallib,
        context_log=args.context_log,
        draw_context=args.draw_context,
        corpus=args.corpus,
        archives=args.archives,
        embedded=args.embedded,
        corpus_report=args.corpus_report,
        split_buckets=args.split_bucket,
        app_libraries=tuple(_parse_app_library(spec) for spec in args.app_library),
    )
    return inputs, args.out


def main() -> None:
    inputs, out = _parse_args()
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(build_tar(inputs))


if __name__ == '__main__':
    main()
