"""Who had the CPU: busy cores per 10 s, the processes behind them, and how long the frame's threads waited for a core.

Busy cores come from SwitchPower (every debug boot); processes from probes-kernel's SwitchSched cores and tasks lines
when the kernel has them; runnable-to-running from SwitchSched 1/2 and 2/2; where the time went inside a process from
kprof, a measurement kernel's lines.
"""

import re
from collections import Counter
from dataclasses import dataclass
from datetime import datetime, timedelta

from tools.devicerun.cpu import windows as task_windows
from tools.perf.source import Rec
from tools.perf.windows import Window

POWER = re.compile(r'^SwitchPower: (?P<ms>\d+) ms: ')
CORE_BUSY = re.compile(r'cpu\d c7 [\d.]+% \([^)]*\) wfi [\d.]+% busy (?P<busy>[\d.]+)%')
EMC = re.compile(r'EMC \d+ moves (?P<rates>(?:\d+:[\d.]+% ?)+)')
EMC_RATE = re.compile(r'(?P<mhz>\d+):(?P<share>[\d.]+)%')
THREAD = re.compile(
    r'(?P<name>[\w.\-]+)\[(?P<pid>\d+)\] [^:;]*: (?P<runs>\d+) runs (?P<p50>\d+)/(?P<p99>\d+)/(?P<max>\d+) us'
)
KPROF_KEEP = ('SwitchKprof tasks', 'SwitchKprof user', 'SwitchKprof waits', 'SwitchKprof images')


@dataclass
class Busy:
    end: datetime
    ms: int
    cores: float
    emc: dict[int, float]  # the memory clock's residency in the window: MHz: share


@dataclass
class Waits:
    runs: int = 0
    worst_p99: int = 0
    worst_us: int = 0
    when: datetime | None = None  # the 10 s window that had the longest wait
    booting: bool = False  # that window ended before System Boot Complete


def busy_windows(recs: list[Rec]) -> list[Busy]:
    out = []
    for r in recs:
        if r.proc == 'kernel' and (m := POWER.match(r.msg)):
            cores = sum(float(c['busy']) for c in CORE_BUSY.finditer(r.msg)) / 100
            rates = EMC.search(r.msg)
            emc = {int(e['mhz']): float(e['share']) / 100 for e in EMC_RATE.finditer(rates['rates'])} if rates else {}
            out.append(Busy(r.time, int(m['ms']), cores, emc))
    return out


def mean_busy(window: Window, busy: list[Busy]) -> float | None:
    """Return the busy cores of the 10 s windows over the window, weighted by how much of each lies inside it."""
    weights = [(b, window.overlap(b.end - timedelta(milliseconds=b.ms), b.end)) for b in busy]
    total = sum(w for _, w in weights)
    return sum(b.cores * w for b, w in weights) / total if total else None


def mean_emc(window: Window, busy: list[Busy]) -> dict[int, float]:
    """Return the memory clock's residency over the window (MHz: share), weighted like mean_busy."""
    shares: dict[int, float] = {}
    total = 0.0
    for b in busy:
        w = window.overlap(b.end - timedelta(milliseconds=b.ms), b.end)
        total += w if b.emc else 0
        for mhz, share in b.emc.items():
            shares[mhz] = shares.get(mhz, 0.0) + share * w
    return {mhz: s / total for mhz, s in shares.items()} if total else {}


def tasks(recs: list[Rec]) -> list:
    """Return probes-kernel's windows (tools/devicerun/cpu.py), each with its processes' user + system ms."""

    def fields(r: Rec) -> dict:
        return {'message': r.msg, 'time': r.time, 'boot': r.boot}

    cores = [fields(r) for r in recs if r.proc == 'kernel' and r.msg.startswith('SwitchSched cores (')]
    lines = [fields(r) for r in recs if r.proc == 'kernel' and r.msg.startswith('SwitchSched tasks (')]
    return task_windows(cores, lines)


def top_tasks(window: Window, ws: list, n: int = 8) -> list[tuple[str, float]]:
    """Return the processes by user + system ms per second over the task windows ending inside the window."""
    used: Counter[str] = Counter()
    seconds = 0.0
    for w in ws:
        if window.holds(w.time) and w.tasks:
            used.update(w.tasks)
            seconds += w.ms / 1000
    return [(name, total / seconds) for name, total in used.most_common(n)] if seconds else []


def core_waits(recs: list[Rec]) -> dict[str, Waits]:
    """Return, per named thread, its runs and its worst runnable-to-running p99 and maximum over the session."""
    out: dict[str, Waits] = {}
    for r in recs:
        if r.proc != 'kernel' or not r.msg.startswith(('SwitchSched 1/2', 'SwitchSched 2/2')):
            continue
        for m in THREAD.finditer(r.msg):
            w = out.setdefault(m['name'], Waits())
            w.runs += int(m['runs'])
            w.worst_p99 = max(w.worst_p99, int(m['p99']))
            if int(m['max']) > w.worst_us:
                w.worst_us, w.when, w.booting = int(m['max']), r.time, 'no System Boot Complete yet' in r.msg
    return out


def kprof(recs: list[Rec]) -> tuple[str, list[Rec]]:
    """Return kprof's state in this boot (off, absent or on) and its per-window lines about processes."""
    lines = [r for r in recs if r.proc == 'kernel' and r.msg.startswith('SwitchKprof')]
    if not lines:
        return 'absent (no SwitchKprof line: a release kernel, or the archive does not reach the boot)', []
    if all(r.msg.startswith('SwitchKprof: off') for r in lines):
        return 'off (built without --//kernel:kprof)', []
    return 'on', [r for r in lines if r.msg.startswith(KPROF_KEEP)]
