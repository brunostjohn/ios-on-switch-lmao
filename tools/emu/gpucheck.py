"""check.py's `gpu` milestone: the GM20B kext's bring-up and the kernel's own channel traffic on the emulated GPU.

It is reached at the boot pool scrub's queued line once M3 and M4 have passed: by then the self-test channel, the
copy-engine fill it checks and the scrubber's channel have run on the model (patch 0030 runs a channel's work when
its GP_PUT is written). A bring-up FAIL line, a report of nvkm's or the kext's that only a GPU error produces, or a
fault or stopped channel in the model's own log fails it, as does a run in which the GPU executed no method.
"""

import re
from collections.abc import Sequence
from pathlib import Path

from tools.emu import gpulog

REACHED = re.compile(r'tegra-gm20b: pool scrub: \d+ MiB queued')
REQUIRED = (re.compile(r'tegra-gm20b: M3 ok'), re.compile(r'tegra-gm20b: M4 ok'))
FAILURES = re.compile(
    r'tegra-gm20b: M\d FAIL'
    r'|tegra-gm20b WARN '
    r'|pool scrub unavailable'
    r'|57000000\.gpu: \w+:[0-9a-f]+: !'
    r'|PBDMA\d+: '
    r'|fault [0-9a-f]{2} \[\w+\] at '
    r'|rc scheduled'
    r'|channel killed'
    r'|GPU error in '
    r'|GPU hang'
    r'|wait for idle timeout'
    r'|GPU scheduler error'
    r'|GPU bind error'
)


def failure(log: Sequence[str]) -> str | None:
    return next((line for line in log if FAILURES.search(line)), None)


def reached(log: Sequence[str]) -> str | None:
    if not all(any(required.search(line) for line in log) for required in REQUIRED):
        return None
    return next((line for line in log if REACHED.search(line)), None)


def after_run(run: Path) -> list[str]:
    """Return what the model's logs (complete once QEMU has quit) hold against the milestone."""
    problems = []
    raw = run / 'gpu-methods.raw'
    methods = sum(1 for line in raw.open() if line.strip()) if raw.exists() else 0
    if not methods:
        problems.append('the GPU executed no method')
    problems += gpulog.fault_lines(run / 'gpu-events.log')
    return problems
