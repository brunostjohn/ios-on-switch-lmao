"""One session on the device, one report: pull the log archive and the reports over USB, then attribute what was slow.

`pull` copies the running Switch's log archive (`idevicesyslog archive`) and its crash reports (`idevicecrashreport`)
into a new folder and writes the report there; `report` reads an archive (or ndjson files) already on disk.
"""

import argparse
import shutil
import subprocess
import sys
import tarfile
from datetime import UTC, datetime
from pathlib import Path

from tools.perf import ips, report, session, source


def pull_commands(out: Path, udid: str | None, age_s: int) -> list[list[str]]:
    """Return the commands that pull one session into out: the log archive as a tar, then the crash folder."""
    device = ['-u', udid] if udid else []
    return [
        ['idevicesyslog', *device, 'archive', str(out / 'archive.tar'), '--age-limit', str(age_s)],
        ['idevicecrashreport', *device, '-k', '-e', str(out / 'crash')],
    ]


def pull(out: Path, udid: str | None, age_s: int) -> Path:
    """Pull a session into a new folder under out and return its .logarchive."""
    folder = out / datetime.now(UTC).strftime('%Y%m%d-%H%M%S')
    folder.mkdir(parents=True)
    for cmd in pull_commands(folder, udid, age_s):
        if not shutil.which(cmd[0]):
            raise SystemExit(f'{cmd[0]} is not installed (brew install libimobiledevice)')
        subprocess.run(cmd, check=True)
    staging = folder / 'archive'
    staging.mkdir()
    with tarfile.open(folder / 'archive.tar') as tar:
        tar.extractall(staging, filter='data')
    (folder / 'archive.tar').unlink()
    archive = folder / 'boot.logarchive'
    archive_root(staging).rename(archive)
    if staging.exists():
        staging.rmdir()
    return archive


def archive_root(folder: Path) -> Path:
    """Return the log archive in an extracted pull: the folder itself, or the one folder in it, holding Info.plist."""
    if (folder / 'Info.plist').exists():
        return folder
    inside = [d for d in folder.iterdir() if d.is_dir() and (d / 'Info.plist').exists()]
    if len(inside) != 1:
        raise SystemExit(f'no log archive in {folder}: no Info.plist at its top or one level down')
    return inside[0]


def write_reports(paths: list[str], crashes: str | None, args: argparse.Namespace) -> list[str]:
    recs, images = source.read(paths, args.start, args.end)
    reports = ips.scan(crashes)
    spins = ips.spin_files(crashes)
    texts = []
    for boot, boot_recs in source.by_boot(recs).items():
        if not boot or (args.boot and not boot.upper().startswith(args.boot.upper())):
            continue
        texts.append(report.render(session.build(boot, boot_recs), reports, images, spins))
    return texts


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest='command', required=True)
    rep = sub.add_parser('report', help='report on archives already pulled')
    rep.add_argument('logs', nargs='+', help='.logarchive folders (absolute under bazel run) or ndjson files')
    rep.add_argument('--crashes', help='the pulled crash folder (idevicecrashreport -e)')
    pul = sub.add_parser('pull', help='pull a session from the device over USB, then report on it')
    pul.add_argument('folder', help='where the session folder is made (absolute under bazel run)')
    pul.add_argument('--udid', help='the device, when more than one is plugged in')
    pul.add_argument('--age', type=int, default=3600, help='seconds of log to pull (default 3600)')
    for p in (rep, pul):
        p.add_argument('--out', help='write the report here instead of printing it')
        p.add_argument('--boot', help='only the boot whose UUID starts with this')
        p.add_argument('--start', help="log show's --start, e.g. '2026-11-23 00:15:00'")
        p.add_argument('--end', help="log show's --end")
    args = parser.parse_args(argv)
    if args.command == 'pull':
        archive = pull(Path(args.folder), args.udid, args.age)
        logs, crashes = [str(archive)], str(archive.parent / 'crash')
        args.out = args.out or str(archive.parent / 'report.txt')
    else:
        logs, crashes = args.logs, args.crashes
    text = '\n'.join(write_reports(logs, crashes, args))
    if args.out:
        Path(args.out).write_text(text)
        print(f'report: {args.out}')
    else:
        print(text, end='')
    return 0 if text else 1


if __name__ == '__main__':
    sys.exit(main())
