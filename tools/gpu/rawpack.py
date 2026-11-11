#!/usr/bin/env python3
"""Make the raw GM20B path's pipeline pack (drivers/TegraGM20BMetal/raw/pack/rawpack.py) beside the seed.

One action per step (gpu/seed/tools.bzl seed_tools builds, gpu/seed/defs.bzl seed_raw_pack runs the rest):

    rawpack.py build --xcode-env F --host-bridge <darwin bridge .a> --generator <raw/pack/rawpack.py> --tree <tree>
                     --out <dir>
        the generator's host tools (directpack and the probes through this build's frontend and host bridge, the mock
        GM20B, the raw path's own.metallib) into <dir>
    rawpack.py corpus --corpus corpus.log --build <IPSW build> [--archives <build>.tsv] --out <file> --report <file>
        the corpus lines the probe makes (raw/pack/probe.mm: Core Animation's render states, without lib=, and every
        other framework's the runtime looks up in the pack, framework_line), so an edit to any other line leaves the
        run's input as it was; with the render states Apple's archives declare for the
        device (precompile/archives/<build>.tsv) whose functions are one framework's default.metallib. A line journaled
        under another build (os=<build> in its head) is left out; one with no os= is taken, and both are counted in
        <report>, which the link puts at the end of the pack's report
    rawpack.py split --generator <rawpack.py> --qc default.metallib --corpus <file> [--ca-names <file>]
                     --out <bucket corpus>...
        the states of each of the generator's buckets, with the Core Animation states a device's logs named
        (precompile/ca-names/<build>.txt)
    rawpack.py probe --generator <rawpack.py> --raw <dir> --nvk <twin dylib> --qc default.metallib
                     --lines <bucket corpus> --out <dir named bucket<i>>
        one bucket's states and kernels made directly through the shader tools (raw/ holds directpack, the probe, the
        mock and own.metallib; the NVK twin), with bucket 0's warm-up draw over the mock GM20B
    rawpack.py link --generator <rawpack.py> --out <dir> [--corpus-report <file>] <bucket dir>...
        the pack made from the buckets' records, unstamped: <out>/pack.bin and report.txt
    rawpack.py stamp --generator <rawpack.py> --pack <pack.bin> --manifest <MANIFEST.json> --out <dir>
        <out>/pack.bin with the tools' stack key in its header

Only the stamp reads the stack key, which every raw/ source changes: a raw runtime edit reruns the stamp alone (but for
the direct compile's own sources, which the tools build from), and a bucket runs again only when its states, the tools,
the NVK twin or QuartzCore's library change. No step but build
runs a compiler, so the image builder runs them with the release's prebuilt tools.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

PROBE_LABEL = ' label=com.apple.coreanimation.'
LIBRARY = re.compile(r' lib=\S+')
# A function a state links (" | <vl|vp|fl|fp> <function> lib=<library> ..."), which any library may hold
LINKED_LIBRARY = re.compile(r'( \| [vf][lp] \S+) lib=\S+')
# What the link reads of a bucket (directlink.py)
KEPT = ('records.bin', 'warm.out', 'warm.log')
FRAMEWORK_LIBRARY = re.compile(r' lib=/System/Library/(?:Private)?Frameworks/(\w+)\.framework/default\.metallib')
STAGE_LIBRARY = re.compile(r' \| [vf] \S+( lib=\S+)')
# The OS build a line was journaled under, in its head: an R line's (before " | v ") or a D line's
OS_BUILD = re.compile(r' os=(\S+)')


def framework_line(line: str) -> bool:
    """Whether the runtime looks the line's state up in the pack under its framework (RawMetalStates.mm _packable).

    A vertex and a fragment function of one framework's default.metallib, and every function it links a framework
    library's; not QuartzCore's, whose states the pack makes from their names (rawpack.py ca_states).
    """
    stages = STAGE_LIBRARY.findall(line)
    frameworks = {m.group(1) for lib in stages if (m := FRAMEWORK_LIBRARY.fullmatch(lib))}
    linked = [lib for lib in LIBRARY.findall(line) if lib not in stages]
    return (
        len(stages) == 2
        and len(set(stages)) == len(frameworks) == 1
        and frameworks != {'QuartzCore'}
        and all(FRAMEWORK_LIBRARY.fullmatch(lib) for lib in linked)
    )


def probe_lines(corpus: str) -> list[str]:
    """Return the corpus lines directpack.mm and probe.mm make, as they read them (a D line keeps its libraries)."""
    return [
        line if framework_line(line) or line.startswith('D ') else LIBRARY.sub('', line)
        for line in corpus.split('\n')
        if (line.startswith('R ') and ' ok=1 ' in line and (PROBE_LABEL in line or framework_line(line)))
        or line.startswith('D ok=1 ')
    ]


def own_build(lines: list[str], build: str) -> tuple[list[str], list[str]]:
    """Give the lines a pack for IPSW `build` takes, without their os= tag, and the report's lines.

    A line tagged with another build is that build's: its encodings (RenderBox's c1 format, QuartzCore's constant sets)
    are not this one's, so it is left out. An untagged line names no build, and no build is guessed for it: it is
    taken, and counted.
    """
    taken, other, untagged = [], {}, 0
    for line in lines:
        tag = OS_BUILD.search(line.split(' | ', 1)[0])
        if tag and tag[1] != build:
            other[tag[1]] = other.get(tag[1], 0) + 1
            continue
        untagged += tag is None
        taken.append(OS_BUILD.sub('', line, count=1) if tag else line)
    builds = ', '.join(f'{b} {n}' for b, n in sorted(other.items())) or 'none'
    taken_text = f'corpus for {build}: {len(taken)} lines taken, {untagged} of them untagged (no os=)'
    return taken, [f'{taken_text}; {sum(other.values())} of another build left out ({builds})']


def archive_lines(inventory: str) -> list[str]:
    """Return the render states an archive inventory (corpus/tools/archives/declared.py) has the pack make.

    Those declared for the raw device's GPU families that the runtime looks up in the pack (framework_line);
    QuartzCore's archive is its offline compilation manifest (raw/pack/ca/caset.py's manifest tier). They keep lib=,
    which the probe loads them from.
    """
    out = []
    for row in inventory.splitlines():
        kind, _owner, _source, family, line = row.split('\t')
        if kind == 'render' and family == 'device' and framework_line(line):
            out.append(line)
    return out


def generate(generator: list[str], arguments: list[str | Path], env: dict[str, str], what: str) -> str:
    """Run the generator; exit with its output's tail if it fails."""
    result = subprocess.run([*generator, *map(str, arguments)], env=env, capture_output=True, text=True, check=False)
    if result.returncode:
        sys.exit(f'{what} failed:\n{(result.stdout + result.stderr)[-6000:]}')
    return result.stdout


def fresh(path: Path) -> Path:
    """Return `path` as an empty directory."""
    shutil.rmtree(path, ignore_errors=True)
    path.mkdir(parents=True)
    return path


def build(args: argparse.Namespace, generator: list[str], env: dict[str, str], root: Path) -> None:
    xcode = dict(line.split('=', 1) for line in Path(args.xcode_env).read_text().splitlines() if '=' in line)
    env['DEVELOPER_DIR'] = xcode['DEVELOPER_DIR']
    arguments = ['--build-only', '--bridge', root / args.host_bridge, '--metal-abi', root / args.metal_abi]
    arguments += ['--out', root / args.out]
    generate(generator, arguments, env, "the pack generator's tools")


def split(args: argparse.Namespace, generator: list[str], env: dict[str, str], work: Path) -> None:
    arguments = ['--split', '--qc', Path(args.qc).resolve(), '--corpus', Path(args.corpus).resolve(), '--out', work]
    arguments += ['--ca-names', Path(args.ca_names).resolve()] if args.ca_names else []
    print(generate(generator, arguments, env, 'the split into buckets').rstrip())
    made = sorted((work / 'work').glob('bucket*'), key=lambda d: int(d.name.removeprefix('bucket')))
    if len(made) != len(args.out):
        sys.exit(f'the generator made {len(made)} buckets, the build declares {len(args.out)}')
    for bucket, out in zip(made, args.out, strict=True):
        shutil.copyfile(bucket / 'corpus.log', out)


def probe(args: argparse.Namespace, generator: list[str], env: dict[str, str], work: Path) -> None:
    out = fresh(Path(args.out).resolve())
    shutil.copyfile(args.lines, out / 'corpus.log')
    # The probe opens NVK from a copy of its own: an input is a link to the output base's file, which dyld can name by
    # another of its links.
    dylib = work / 'libvulkan_nouveau.dylib'
    shutil.copyfile(args.nvk, dylib)
    dylib.chmod(0o755)
    libraries = [Path(p) for p in args.data]
    if args.data_dir:
        libraries += sorted(Path(args.data_dir).glob('*.metallib'))
    data = library_view(libraries, work / 'data')
    env = dict(env, PROBE_DATA_LIBS=str(data.resolve()))
    arguments = ['--probe-bucket', out, '--prebuilt', Path(args.raw).resolve(), '--nvk', dylib]
    print(generate(generator, [*arguments, '--qc', Path(args.qc).resolve()], env, f'making {out.name}').rstrip())
    for path in out.iterdir():
        if path.is_file() and path.name not in KEPT:
            path.unlink()


def library_view(libraries: list[Path], destination: Path) -> Path:
    destination.mkdir()
    for source in libraries:
        digest = hashlib.sha256(source.read_bytes()).hexdigest()
        target = destination / (digest + '.metallib')
        if not target.exists():
            target.symlink_to(source.resolve())
    return destination


def link(args: argparse.Namespace, generator: list[str], env: dict[str, str], work: Path) -> None:
    linked = fresh(work / 'linked')
    report = generate(generator, ['--link', '--out', linked, *[Path(b).resolve() for b in args.buckets]], env, 'link')
    if args.corpus_report:
        corpus = Path(args.corpus_report).read_text()
        report += corpus
        with open(linked / 'report.txt', 'a') as f:
            f.write(corpus)
    # Every state left out has its own line, megabytes of them: Bazel shows none of an action's output past 1 MiB, so
    # the summary comes here and the lines stay in report.txt (the link's output group).
    skipped = [line for line in report.splitlines() if line.startswith('  skipped')]
    print('\n'.join(line for line in report.splitlines() if not line.startswith('  skipped')))
    print(f'{len(skipped)} states left out, each named in report.txt')
    out = fresh(Path(args.out))
    shutil.copyfile(linked / 'pack.bin', out / 'pack.bin')
    shutil.copyfile(linked / 'report.txt', out / 'report.txt')


def stamp(args: argparse.Namespace, generator: list[str], env: dict[str, str], _work: Path) -> None:
    stack = json.loads(Path(args.manifest).read_text())['stack']
    out = fresh(Path(args.out))
    shutil.copyfile(args.pack, out / 'pack.bin')
    generate(generator, ['--stamp', out / 'pack.bin', '--stack', stack], env, 'the stamp')
    print(f'raw pack: stack {stack}')


STEPS = {
    'build': ('xcode-env', 'host-bridge', 'metal-abi', 'generator', 'tree', 'out'),
    'split': ('generator', 'qc', 'corpus'),
    'probe': ('generator', 'raw', 'nvk', 'qc', 'lines', 'out'),
    'link': ('generator', 'out'),
    'stamp': ('generator', 'pack', 'manifest', 'out'),
}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    steps = parser.add_subparsers(dest='step', required=True)
    corpus = steps.add_parser('corpus')
    for name in ('corpus', 'out'):
        corpus.add_argument('--' + name, required=True)
    corpus.add_argument('--archives')
    corpus.add_argument('--build', required=True)
    corpus.add_argument('--report', required=True)
    for step, names in STEPS.items():
        p = steps.add_parser(step)
        for name in names:
            p.add_argument('--' + name, required=True)
    steps.choices['split'].add_argument('--out', nargs='+', required=True)
    steps.choices['split'].add_argument('--ca-names')
    steps.choices['probe'].add_argument('--data-dir')
    steps.choices['probe'].add_argument('--data', action='append', default=[])
    steps.choices['link'].add_argument('buckets', nargs='+')
    steps.choices['link'].add_argument('--corpus-report')
    args = parser.parse_args()
    if args.step == 'corpus':
        lines, report = own_build(probe_lines(Path(args.corpus).read_text()), args.build)
        if args.archives:
            harvested = set(lines)
            lines += [line for line in archive_lines(Path(args.archives).read_text()) if line not in harvested]
        Path(args.out).write_text(''.join(line + '\n' for line in lines))
        Path(args.report).write_text(''.join(line + '\n' for line in report))
        print('\n'.join(report))
        return
    root = Path.cwd()
    work = fresh(root / f'_rawpack_{args.step}_{os.getpid()}')
    (work / 'home').mkdir()
    env = {
        'PATH': '/usr/bin:/bin',
        'ZERO_AR_DATE': '1',
        'HOME': str(work / 'home'),
        'TMPDIR': os.environ.get('TMPDIR', str(work)),
        'RAWPACK_TREE': str(root / getattr(args, 'tree', '.')),
    }
    generator = [sys.executable, str(root / args.generator)]
    {'build': build, 'split': split, 'probe': probe, 'link': link, 'stamp': stamp}[args.step](
        args, generator, env, root if args.step == 'build' else work
    )
    shutil.rmtree(work)


if __name__ == '__main__':
    main()
