"""Every lane's owed device checks in one place: their groups for checks.py, and the summary rows devicerun prints."""

from tools.devicerun import owed, owed_gpu, owed_system
from tools.devicerun.rules import NO_DATA, REPORT, Check, Result

ORDER = (
    'frame',
    'memory',
    'state',
    'boot',
    'dma',
    'display',
    'power',
    'gputhroughput',
    'gpuformats',
    'gpurender',
    'hwtest2',
    'fetchmsaa',
    'shadergen',
    'devicegates',
    'nohash',
    'nvcompiler',
    'measure',
)
LANES = {lane: {**owed_system.LANES, **owed_gpu.LANES}[lane] for lane in ORDER}
# Lanes whose owed checks could not be read: no checks, a NO-DATA row naming why.
MISSING = {'motion': 'no review-motion4 notes at out/agent-scratch/c38-audit/perf1/review-motion4 (25 Nov)'}
GROUPS = {f'owed-{lane}': owed.checks(entries) for lane, entries in LANES.items()}


def summary(by_check: dict[str, Result], boots: list[str]) -> list[Result]:
    """The archive's boots, then one row per lane: FAIL if any of its checks failed, else PASS if any passed."""
    rows = [owed.archive_row(boots)]
    rows += [owed.lane_row(lane, entries, by_check) for lane, entries in LANES.items()]
    rows += [Result(Check(f'owed.{lane}', f'big-{lane}', '', REPORT), NO_DATA, why) for lane, why in MISSING.items()]
    return rows
