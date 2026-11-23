"""The flight recorder's ring (kernel/switch/recorder): a dump's RAW kdebug file as events, and what the events say.

kdbg_dump_trace_to_file writes a RAW_VERSION1 header, a thread map (none here: the stackshot beside the file names
the threads), padding to a 4 KiB boundary that carries the CPU map, then 64-byte events oldest first: the mach time,
four arguments, the thread that logged it, the debug id and the CPU.
"""

import struct
from collections import defaultdict
from dataclasses import dataclass, field

RAW_VERSION1 = 0x55AA0101
HEADER = struct.Struct('<iiQI4x')
THREADMAP_ENTRY = 32
EVENT = struct.Struct('<QQQQQQIIQ')
PAGE = 4096
FUNC_START, FUNC_END = 1, 2


def code(cls: int, sub: int, number: int) -> int:
    return (cls << 24) | (sub << 16) | (number << 2)


# bsd/sys/kdebug.h, osfmk/kperf/buffer.h, kernel/switch/include/switch/recorder.h and
# TegraDisplayFlip/SwitchFrameTimeline.h
MACH_SCHED = code(1, 0x40, 0)
MACH_MAKE_RUNNABLE = code(1, 0x40, 6)
PERF_TI_DATA = code(37, 1, 1)
PERF_CS_KDATA, PERF_CS_UDATA = code(37, 2, 3), code(37, 2, 4)
PERF_CS_KHDR, PERF_CS_UHDR = code(37, 2, 5), code(37, 2, 6)
TRIGGER, DUMP, GPU_JOB = code(6, 18, 0x3E0), code(6, 18, 0x3E1), code(6, 18, 0x3E8)
SUBMIT = code(6, 18, 0x3F1)
FRAME_POINTS = {
    code(6, 18, 0x3F0 + i): name
    for i, name in enumerate(
        ('vsync', 'submit', 'gpu done', 'swap', 'present', 'present done', 'flip', 'latch', 'frame ticket')
    )
}
REASONS = ('none', 'late frame', 'hang', 'slow work', 'mark')


@dataclass(frozen=True)
class Event:
    ts: int
    args: tuple[int, int, int, int]
    tid: int
    debugid: int
    cpu: int

    @property
    def id(self) -> int:
        return self.debugid & ~3

    @property
    def func(self) -> int:
        return self.debugid & 3


@dataclass
class Ring:
    tod_secs: int
    events: list[Event]


@dataclass
class Sample:
    ts: int
    tid: int
    kernel: bool
    frames: list[int] = field(default_factory=list)


@dataclass(frozen=True)
class Job:
    ts: int
    channel: int
    seqno: int
    compositor: bool
    start_ns: int
    end_ns: int


def parse(data: bytes) -> Ring:
    version, threads, secs, _usecs = HEADER.unpack_from(data, 0)
    if version != RAW_VERSION1:
        raise ValueError(f'not a RAW_VERSION1 kdebug file (version 0x{version & 0xFFFFFFFF:x})')
    start = -(-(HEADER.size + threads * THREADMAP_ENTRY) // PAGE) * PAGE
    end = start + (len(data) - start) // EVENT.size * EVENT.size
    events = [
        Event(ts, (a1, a2, a3, a4), tid, debugid, cpu)
        for ts, a1, a2, a3, a4, tid, debugid, cpu, _ in EVENT.iter_unpack(data[start:end])
        if ts
    ]
    events.sort(key=lambda e: e.ts)
    return Ring(secs, events)


def tod(path: str) -> int:
    """Return the wall time (s) a dump's ring was written, from its header alone."""
    with open(path, 'rb') as f:
        head = f.read(HEADER.size)
    return HEADER.unpack(head)[2] if len(head) == HEADER.size else 0


def read(path: str) -> Ring:
    with open(path, 'rb') as f:
        return parse(f.read())


def runs(events: list[Event]) -> dict[int, list[tuple[int, int, int]]]:
    """Return each thread's stretches on a core, (start, end, cpu), from the context switches."""
    on: dict[int, tuple[int, int]] = {}  # cpu: (thread, since)
    out: dict[int, list[tuple[int, int, int]]] = defaultdict(list)
    first = events[0].ts if events else 0
    for e in events:
        if e.id != MACH_SCHED:
            continue
        old_tid, since = on.get(e.cpu, (e.tid, first))
        out[old_tid].append((since, e.ts, e.cpu))
        on[e.cpu] = (e.args[1], e.ts)
    last = events[-1].ts if events else 0
    for cpu, (tid, since) in on.items():
        out[tid].append((since, last, cpu))
    return out


def on_core(stretches: list[tuple[int, int, int]], lo: int, hi: int) -> int:
    return sum(max(0, min(b, hi) - max(a, lo)) for a, b, _ in stretches)


def wakers(events: list[Event], tid: int, lo: int, hi: int) -> dict[int, int]:
    """Return who made the thread runnable in [lo, hi], and how often."""
    out: dict[int, int] = defaultdict(int)
    for e in events:
        if e.id == MACH_MAKE_RUNNABLE and e.args[0] == tid and lo <= e.ts <= hi:
            out[e.tid] += 1
    return out


def samples(events: list[Event]) -> list[Sample]:
    """Return kperf's stack samples: a header with the frame count, then its frames four to an event."""
    pending: dict[tuple[int, bool], tuple[Sample, int]] = {}
    out = []
    for e in events:
        if e.id in {PERF_CS_KHDR, PERF_CS_UHDR}:
            kernel = e.id == PERF_CS_KHDR
            pending[(e.tid, kernel)] = (Sample(e.ts, e.tid, kernel), e.args[1])
        elif e.id in {PERF_CS_KDATA, PERF_CS_UDATA}:
            key = (e.tid, e.id == PERF_CS_KDATA)
            if key not in pending:
                continue
            sample, want = pending[key]
            sample.frames += [f for f in e.args if f][: want - len(sample.frames)]
            if len(sample.frames) >= want or not all(e.args):
                out.append(sample)
                del pending[key]
    return out


def trigger(events: list[Event]) -> tuple[int, str, int, int] | None:
    """Return the dump's trigger: the last before the dump began (time, reason, thread, value)."""
    found = None
    for e in events:
        if e.id == TRIGGER:
            found = (e.ts, REASONS[e.args[0]] if e.args[0] < len(REASONS) else str(e.args[0]), e.args[1], e.args[2])
        elif e.id == DUMP and e.func == FUNC_START:
            break
    return found


def jobs(events: list[Event]) -> tuple[list[Job], dict[int, int]]:
    """Return the GPU's completed jobs and each channel's submitting thread."""
    out, submitter = [], {}
    for e in events:
        if e.id == GPU_JOB:
            a = e.args[0]
            out.append(Job(e.ts, a & 0xFFFF, a >> 32, bool((a >> 16) & 1), e.args[1], e.args[2]))
        elif e.id == SUBMIT:
            submitter[e.args[0]] = e.tid
    return out, submitter
