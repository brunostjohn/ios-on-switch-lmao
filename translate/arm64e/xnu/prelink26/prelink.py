#!/usr/bin/env python3
"""The kext prelink of a fileset kernelcache (iPadOS 26/27) into our 12377 kernel, as one build step.

The kexts of the roots' closure, translated in kernel mode and linked against our kernel by name, in the main tree's
prelink format (emit26.py). Runs classify.py, pcrel.py, names.py, emit26.py and verify26.py over a work directory
holding the ipsw CLI's views of the kernelcache.

usage: prelink.py --kernel K --kernelcache KC --kernelcache-17 KC17 --arm64e CLI --ipsw IPSW --symbolsets PLIST
                  --signatures VERSION/xnu.json --slide SLIDE --out OUT --report REPORT --names NAMES
                  --bindings BINDINGS --root BUNDLE... [--cut COMPONENT=DEPENDENCY=SITES...]
                  [--defer COMPONENT=PERSONALITY...]
       prelink.py --from-bindings BINDINGS --kernel K --kernelcache KC --arm64e CLI [--sites SITES...]
                  --expect PRELINKED --rebuilt REBUILT --out OUT

SLIDE is the slide the drivers' bindings (kits/SwitchABI's tables) assume: a prelink that moves the kexts elsewhere
fails here instead of linking addresses that miss.

The second form prelinks from BINDINGS (kernel_bindings.py) alone, with none of the naming's inputs, and copies
BINDINGS to OUT only when that kernel is PRELINKED byte for byte.
"""

import argparse
import hashlib
import json
import os
import plistlib
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).absolute().parent


def run(env: dict[str, str], *argv: object, stdout: object = None) -> None:
    subprocess.run([str(a) for a in argv], env=env, check=True, stdout=stdout)


def symbol_set_names(plist: Path) -> list[str]:
    """Every name and prefix our kernel's symbol sets export (symbolsets.plist): the KPI names a kext may import."""
    sets = plistlib.loads(plist.read_bytes())['SymbolsSets']
    names = {(e.get('SymbolName') or e.get('SymbolPrefix') or '').strip() for s in sets for e in s.get('Symbols', [])}
    return sorted(n for n in names if n)


def prelink(argv: list[str]) -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in (
        'kernel',
        'kernelcache',
        'kernelcache-17',
        'arm64e',
        'ipsw',
        'symbolsets',
        'signatures',
        'out',
        'report',
        'names',
        'bindings',
    ):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--slide', required=True, type=lambda v: int(v, 0))
    parser.add_argument('--root', required=True, action='append')
    parser.add_argument('--cut', action='append', default=[], type=lambda v: v.split('='))
    parser.add_argument('--defer', action='append', default=[], type=lambda v: v.split('='))
    parser.add_argument('--kind', required=True)
    parser.add_argument('--sites', action='append', default=[], type=Path)
    args = parser.parse_args(argv)
    work = Path(tempfile.mkdtemp(prefix='prelink-'))
    env = dict(
        os.environ,
        HOME=str(work),  # ipsw reads its configuration from $HOME: none, here
        PRELINK_WORK=str(work),
        PRELINK_KERNELCACHE=str(args.kernelcache.resolve()),
        PRELINK_OURS=str(args.kernel.resolve()),
        PRELINK_KERNELCACHE_17=str(args.kernelcache_17.resolve()),
        PRELINK_ARM64E=str(args.arm64e.resolve()),
        PRELINK_SLIDE=hex(args.slide),
        PRELINK_KIND=args.kind,
        PRELINK_SITES=os.pathsep.join(str(p.resolve()) for p in args.sites),
        PYTHONPATH=os.pathsep.join([str(HERE), *sys.path]),
    )
    try:
        cuts = [[component, dependency, int(sites)] for component, dependency, sites in args.cut]
        deferred = {}
        for component, personality in args.defer:
            deferred.setdefault(component, []).append(personality)
        lists = {'roots': args.root, 'cuts': cuts, 'deferred': deferred}
        (work / 'prelink-lists.json').write_text(json.dumps(lists))
        nm = subprocess.run(['xcrun', 'nm', str(args.kernel)], env=env, check=True, capture_output=True, text=True)
        (work / 'ours.nm').write_text(''.join(line + '\n' for line in nm.stdout.splitlines() if len(line.split()) == 3))
        with open(work / 'cpp26.json', 'wb') as cpp:
            run(env, args.ipsw, 'kernel', 'cpp', '--json', '--methods', args.kernelcache, stdout=cpp)
        signatures = args.signatures.resolve().parent.parent
        run(
            env,
            args.ipsw,
            'kernel',
            'symbolicate',
            '-q',
            '--json',
            '--signatures',
            signatures,
            '-o',
            work / 'symbolicate',
            args.kernelcache,
        )
        (symbols,) = (work / 'symbolicate').glob('*.symbols.json')
        (work / 'sym').mkdir()
        symbols.rename(work / 'sym' / 'kernelcache.decompressed.symbols.json')
        (work / 'symbolsets26.txt').write_text(''.join(n + '\n' for n in symbol_set_names(args.symbolsets)))
        python = sys.executable
        run(env, python, HERE / 'classify.py', args.kernelcache, stdout=subprocess.DEVNULL)
        run(env, python, HERE / 'pcrel.py', args.kernelcache, stdout=subprocess.DEVNULL)
        run(env, python, HERE / 'names.py', stdout=subprocess.DEVNULL)
        out = work / 'kernel.macho'
        run(
            env,
            python,
            HERE / 'emit26.py',
            '--bindings',
            work / 'prelink-bindings.json',
            args.kernel,
            args.kernelcache,
            out,
        )
        run(env, python, HERE / 'verify26.py', out, args.kernelcache, stdout=subprocess.DEVNULL)
        slide = json.loads((work / 'kernel.macho.json').read_text())['slide']
        if slide != args.slide:
            raise SystemExit(f'the kexts moved by {slide:#x}, the bindings assume {args.slide:#x}')
        shutil.copyfile(out, args.out)
        shutil.copyfile(work / 'kernel.macho.json', args.report)
        shutil.copyfile(work / 'kernel-names.json', args.names)
        shutil.copyfile(work / 'prelink-bindings.json', args.bindings)
    finally:
        shutil.rmtree(work)


def from_bindings(argv: list[str]) -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ('from-bindings', 'kernel', 'kernelcache', 'arm64e', 'expect', 'rebuilt', 'out'):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--sites', action='append', default=[], type=Path)
    args = parser.parse_args(argv)
    env = {k: v for k, v in os.environ.items() if not k.startswith('PRELINK_')}
    env.update(
        PRELINK_ARM64E=str(args.arm64e.resolve()),
        PRELINK_SITES=os.pathsep.join(str(p.resolve()) for p in args.sites),
        PYTHONPATH=os.pathsep.join([str(HERE), *sys.path]),
    )
    with tempfile.TemporaryDirectory(prefix='prelink-') as work:
        out = Path(work) / 'kernel.macho'
        run(
            env,
            sys.executable,
            HERE / 'emit26.py',
            '--from-bindings',
            args.from_bindings,
            args.kernel,
            args.kernelcache,
            out,
        )
        rebuilt = hashlib.sha256(out.read_bytes()).hexdigest()
        expected = hashlib.sha256(args.expect.read_bytes()).hexdigest()
        print(f'from the bindings: {rebuilt}\nprelinked:         {expected}')
        if rebuilt != expected:
            raise SystemExit(f'the prelink from {args.from_bindings} differs from {args.expect}')
        shutil.copyfile(out, args.rebuilt)
    shutil.copyfile(args.from_bindings, args.out)


def main() -> None:
    argv = sys.argv[1:]
    (from_bindings if '--from-bindings' in argv else prelink)(argv)


if __name__ == '__main__':
    main()
