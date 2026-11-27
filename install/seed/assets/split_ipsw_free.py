#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""IPSW-free corpus filtering and bucket split for the shader pipeline pack.

Matches tools/gpu/make_pack.py (corpus + split) using only repository files and
the pinned QuartzCore metallib SHA-256 instead of reading QuartzCore's bytes.
"""

import argparse
import importlib
import json
import os
import re
import shutil
import sys
import tempfile
from pathlib import Path
from typing import NamedTuple

from drivers.TegraGM20BMetal.pack import make_pack as pack_gen

PROBE_LABEL = ' label=com.apple.coreanimation.'
LIBRARY = re.compile(r' lib=\S+')
LINKED_LIBRARY = re.compile(r'( \| [vf][lp] \S+) lib=\S+')
FRAMEWORK_LIBRARY = re.compile(r' lib=/System/Library/(?:Private)?Frameworks/(\w+)\.framework/default\.metallib')
OS_LIBRARY = re.compile(r' lib=/System/Library/\S+?\.(?:bundle|framework/Resources)/\S+\.metallib')
QUARTZCORE = '/QuartzCore.framework/'
STAGE_LIBRARY = re.compile(r' \| [vf] \S+( lib=\S+)')
NO_FRAGMENT = re.compile(r' \| f - ')
APP_LIBRARY = re.compile(r' lib=/Applications/Switch\w*\.app/default\.metallib')
OS_BUILD = re.compile(r' os=(\S+)')
COLOR_FIELDS = re.compile(r' c(\d)=(\d+):(\d+(?::\d+){7})(?= |$)')

YCBCR_TARGETS = frozenset(range(500, 549)) - {542} | {588, 589}
YCBCR_STAND_IN = 125
STAND_INS = {588: 55, 589: 105}
FORMAT_STAND_INS = {11: 71, 31: 71, 41: 125, 42: 125, 44: 125, 84: 70, 85: 71, 93: 125}
HOLDS_ALPHA = frozenset({41, 42, 44, 84, 85})
ALPHA_ONE_RGB = {8: 1, 9: 0, 10: 0}
ALPHA_ONE_ALPHA = {8: 1, 9: 0, 10: 1}


class CorpusInputs(NamedTuple):
    corpus: Path
    build: str
    archives: Path | None
    app_states: Path | None


class SplitInputs(NamedTuple):
    corpus_inputs: CorpusInputs
    qc_sha256: str
    ca_names: Path | None
    ca_preferences: Path
    report: Path
    outs: list[Path]


def framework_line(line: str) -> bool:
    stages = STAGE_LIBRARY.findall(line)
    frameworks = {m.group(1) for lib in stages if (m := FRAMEWORK_LIBRARY.fullmatch(lib))}
    linked = [lib for lib in LIBRARY.findall(line) if lib not in stages]
    if len(stages) != (1 if NO_FRAGMENT.search(line) else 2) or len(set(stages)) != 1:
        return False
    if not frameworks:
        return not LINKED_LIBRARY.search(line) and bool(OS_LIBRARY.fullmatch(stages[0])) and QUARTZCORE not in stages[0]
    return frameworks != {'QuartzCore'} and all(FRAMEWORK_LIBRARY.fullmatch(lib) for lib in linked)


def app_line(line: str) -> bool:
    stages = STAGE_LIBRARY.findall(line)
    return (
        line.startswith('R ok=1 ')
        and len(stages) == 2
        and len(set(stages)) == 1
        and bool(APP_LIBRARY.fullmatch(stages[0]))
        and not LINKED_LIBRARY.search(line)
    )


def probe_lines(corpus: str) -> list[str]:
    return [
        line if framework_line(line) or line.startswith('D ') else LIBRARY.sub('', line)
        for line in corpus.split('\n')
        if (line.startswith('R ') and ' ok=1 ' in line and (PROBE_LABEL in line or framework_line(line)))
        or (line.startswith('D ok=1 ') and not APP_LIBRARY.search(line))
    ]


def alpha_one(fields: str) -> str:
    parts = fields.split(':')
    for idx, table in ((2, ALPHA_ONE_RGB), (3, ALPHA_ONE_RGB), (5, ALPHA_ONE_ALPHA), (6, ALPHA_ONE_ALPHA)):
        parts[idx] = str(table.get(int(parts[idx]), int(parts[idx])))
    return ':'.join(parts)


def stand_in(line: str) -> str:
    head, bar, tail = line.partition(' | ')
    targets = [m for m in COLOR_FIELDS.finditer(head) if int(m[2]) in YCBCR_TARGETS | FORMAT_STAND_INS.keys()]
    if not targets:
        return line
    for match in reversed(targets):
        fmt = int(match[2])
        if fmt in FORMAT_STAND_INS:
            fields = FORMAT_STAND_INS[fmt], match[3] if fmt in HOLDS_ALPHA else alpha_one(match[3])
        else:
            fields = STAND_INS.get(fmt, YCBCR_STAND_IN), alpha_one(match[3])
        head = head[: match.start(2)] + f'{fields[0]}:{fields[1]}' + head[match.end(3) :]
    return head + bar + tail


def stand_in_lines(lines: list[str]) -> list[str]:
    out: list[str] = []
    seen: set[str] = set()
    for line in lines:
        mapped = stand_in(line)
        if mapped == line or mapped not in seen:
            out.append(mapped)
            seen.add(mapped)
    return out


def app_states(text: str) -> list[str]:
    lines = [line.rstrip() + ' ' for line in text.splitlines() if line.strip()]
    for line in lines:
        if not app_line(line):
            sys.exit(f"not one of our own apps' render states, linking nothing: {line[:300]}")
    return lines


def own_build(lines: list[str], build: str) -> tuple[list[str], list[str]]:
    taken: list[str] = []
    named: list[str] = []
    other: dict[str, int] = {}
    untagged: list[str] = []
    for line in lines:
        tag = OS_BUILD.search(line.split(' | ', 1)[0])
        if tag is None:
            untagged.append(line)
            continue
        builds = tag[1].split(',')
        if build in builds:
            taken.append(OS_BUILD.sub('', line, count=1))
            continue
        if line.startswith('R ') and PROBE_LABEL in line:
            named.append(line)
        for other_build in builds:
            other[other_build] = other.get(other_build, 0) + 1
    if untagged:
        sys.exit(f'{len(untagged)} corpus lines name no OS build (os=), the first: {untagged[0][:300]}')
    summary = ', '.join(f'{b} {n}' for b, n in sorted(other.items())) or 'none'
    left = len(lines) - len(taken)
    report = (
        f'corpus for {build}: {len(taken)} lines taken; {left} of other builds left out ({summary}), of them '
        f'{len(named)} Core Animation lines kept for their names'
    )
    return taken + named, [report]


def archive_lines(inventory: str) -> list[str]:
    out: list[str] = []
    for row in inventory.splitlines():
        kind, _owner, _source, family, line = row.split('\t')
        if kind == 'render' and family == 'device' and framework_line(line):
            out.append(line)
    return out


def build_corpus(inputs: CorpusInputs) -> tuple[list[str], list[str]]:
    lines, report = own_build(probe_lines(inputs.corpus.read_text(encoding='utf-8')), inputs.build)
    if inputs.archives:
        harvested = set(lines)
        lines += [
            line.replace(' samples=', ' tier=1 samples=', 1)
            for line in archive_lines(inputs.archives.read_text(encoding='utf-8'))
            if line not in harvested
        ]
    lines = stand_in_lines(lines)
    if inputs.app_states:
        own = app_states(inputs.app_states.read_text(encoding='utf-8'))
        lines += own
        report.append(f"our own apps' states: {len(own)} lines ({inputs.app_states.name})")
    return lines, report


def _find_qc_tables(digest: str) -> Path | None:
    for path in sorted((pack_gen.HERE / 'ca').glob('qc-*.json')):
        with path.open(encoding='utf-8') as handle:
            if json.load(handle).get('metallib_sha256') == digest:
                return path
    return None


def ca_corpus_for_digest(
    digest: str,
    corpus: Path,
    work: Path,
    names: list[str],
    preferences: Path,
) -> Path:
    tables_path = _find_qc_tables(digest)
    if tables_path is None:
        return corpus
    out = work / 'corpus.ca.log'
    lines = corpus.read_text(encoding='utf-8').splitlines(keepends=True)
    ca_dir = str(pack_gen.HERE / 'ca')
    if ca_dir not in sys.path:
        sys.path.insert(0, ca_dir)
    caspec = importlib.import_module('caspec')
    deepcolour = importlib.import_module('deepcolour')
    cif10 = deepcolour.cif10(preferences)
    renderbox, _why = pack_gen.renderbox_tagger(digest)
    with out.open('w', encoding='utf-8') as dst:
        dst.writelines(renderbox(line) for line in lines if not caspec.corpus_fields(line)[0])
        dst.writelines(pack_gen.cold_line(renderbox(line)) + '\n' for line in pack_gen.renderbox_closure(digest, lines))
        for _tier, made in pack_gen.ca_states(tables_path, corpus, names, cif10):
            dst.writelines(line + '\n' for line in made)
    return out


def split_ipsw_free(inputs: SplitInputs) -> None:
    digest = inputs.qc_sha256.strip().lower()
    if len(digest) != 64 or any(ch not in '0123456789abcdef' for ch in digest):
        raise ValueError(f'invalid QuartzCore SHA-256: {inputs.qc_sha256!r}')

    lines, report = build_corpus(inputs.corpus_inputs)
    inputs.report.parent.mkdir(parents=True, exist_ok=True)
    inputs.report.write_text(''.join(line + '\n' for line in report), encoding='utf-8')

    with tempfile.TemporaryDirectory() as scratch:
        work = Path(scratch)
        corpus_path = work / 'corpus.log'
        corpus_path.write_text(''.join(line + '\n' for line in lines), encoding='utf-8')
        names = inputs.ca_names.read_text(encoding='utf-8').split() if inputs.ca_names else []
        expanded = ca_corpus_for_digest(digest, corpus_path, work, names, inputs.ca_preferences.resolve())
        bucket_dirs = pack_gen.split_buckets(expanded, work / 'buckets', pack_gen.BUCKETS)
        if len(bucket_dirs) != len(inputs.outs):
            raise ValueError(f'generator made {len(bucket_dirs)} buckets, build declared {len(inputs.outs)}')
        for bucket_dir, out_path in zip(bucket_dirs, inputs.outs, strict=True):
            out_path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(bucket_dir / 'corpus.log', out_path)


def _parse_args() -> SplitInputs:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--corpus', type=Path, required=True)
    parser.add_argument('--build', required=True)
    parser.add_argument('--qc-sha256', required=True)
    parser.add_argument('--archives', type=Path)
    parser.add_argument('--app-states', type=Path)
    parser.add_argument('--ca-names', type=Path)
    parser.add_argument('--ca-preferences', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--out', type=Path, nargs='+', required=True)
    args = parser.parse_args()
    return SplitInputs(
        corpus_inputs=CorpusInputs(
            corpus=args.corpus,
            build=args.build,
            archives=args.archives,
            app_states=args.app_states,
        ),
        qc_sha256=args.qc_sha256,
        ca_names=args.ca_names,
        ca_preferences=args.ca_preferences,
        report=args.report,
        outs=args.out,
    )


def main() -> None:
    os.environ.setdefault('PYTHONHASHSEED', '0')
    split_ipsw_free(_parse_args())


if __name__ == '__main__':
    main()
