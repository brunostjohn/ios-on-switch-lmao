"""Catalogues and job plists for launchd_catalog's `add_bootstrap_jobs`, each with what it makes of them."""

import copy
import plistlib

from tools.package.launchd_catalog import APPS_JOB_PATH, APPS_LABEL, JOB_PATH, LABEL, add_bootstrap_jobs

REFUSALS = (ValueError, TypeError, KeyError, AttributeError, OverflowError)
SHARED_VOLUME = 'com.brunostjohn.switch.sdwake.shared-volume'

type Case = tuple[bytes, list[bytes], bytes | None]


def _binary(value: object) -> bytes:
    return plistlib.dumps(value, fmt=plistlib.FMT_BINARY, sort_keys=False)


def _xml(value: object) -> bytes:
    return plistlib.dumps(value, sort_keys=False)


def _catalogues() -> list[bytes]:
    plain = {
        'AppExtensions': {'some-extension': {'Path': '/System/Library/test'}},
        'LaunchDaemons': {'/System/Library/LaunchDaemons/apple.plist': {'Label': 'com.apple.fixture'}},
        'SystemLibraryTreeState': {'marker': b'bytes'},
    }
    arguments = ['/usr/libexec/shared', 'root']
    daemons = {
        '/a.plist': {'Label': 'a', 'ProgramArguments': arguments, 'KeepAlive': True, 'UserName': 'root'},
        '/b.plist': {'Label': 'b', 'ProgramArguments': arguments, 'Nice': 1, 'Weight': 1.0, 'Zero': -0.0},
        '/bytes.plist': {'Label': LABEL.encode(), 'Other': APPS_LABEL},
        '/nan.plist': {'Label': 'nan', 'Value': float('nan')},
    }
    shared = {'LaunchDaemons': daemons, 'Alias': daemons, 'Mirror': [daemons, arguments], 'VersionNumber': 2}
    return [_binary(plain), _binary(shared)]


def _refused_catalogues() -> list[bytes]:
    job = {'Label': 'x'}
    return [
        _binary({'LaunchDaemons': {}}),
        _binary({'LaunchDaemons': [job]}),
        _binary({'LaunchDaemons': {'/x.plist': job, '/y.plist': 'not a job'}}),
        _binary({'Other': {'/x.plist': job}}),
        _binary([{'LaunchDaemons': {'/x.plist': job}}]),
        _binary({'LaunchDaemons': {'/x.plist': {'Label': LABEL}}}),
        _binary({'LaunchDaemons': {JOB_PATH: {'Label': 'other'}}}),
        _binary({'LaunchDaemons': {'/x.plist': {'Label': APPS_LABEL}}}),
        _binary({'LaunchDaemons': {APPS_JOB_PATH: job}}),
        b'not a binary plist, though long enough for a trailer',
    ]


def _launch_events(sdwake: dict, stream: object) -> dict:
    events = copy.deepcopy(sdwake['LaunchEvents'])
    events['com.apple.iokit.matching'][SHARED_VOLUME]['IOMatchLaunchStream'] = stream
    return events


def _job_sets(jobs: list[bytes]) -> list[list[bytes]]:
    job, apps, sdwake = (plistlib.loads(data) for data in jobs)
    changes: list[tuple[int, dict]] = [
        (0, {'RunAtLoad': 1}),
        (0, {'ProgramArguments': ['/usr/libexec/switch-bootstrap', '-v']}),
        (0, {'Label': 'other'}),
        (0, {'UserName': 'mobile'}),
        (0, {'Extra': [1.0, -0.0, True]}),
        (1, {'UserName': 'root'}),
        (1, {'MachServices': {APPS_LABEL: 1}}),
        (1, {'MachServices': {APPS_LABEL: True, 'other.service': True}}),
        (1, {'RunAtLoad': True}),
        (1, {'RunAtLoad': False, 'KeepAlive': False}),
        (1, {'KeepAlive': 0}),
        (2, {'UserName': 'mobile'}),
        (2, {'RunAtLoad': True}),
        (2, {'LaunchEvents': {}}),
        (2, {'LaunchEvents': _launch_events(sdwake, 1)}),
        (2, {'LaunchEvents': _launch_events(sdwake, 1.0)}),
        (2, {'LaunchEvents': _launch_events(sdwake, 2)}),
    ]
    sets = [jobs, [_binary(job), _xml(apps), jobs[2]], [jobs[0], _xml(['not', 'a', 'job']), jobs[2]]]
    for which, change in changes:
        changed = list(jobs)
        changed[which] = _xml((job, apps, sdwake)[which] | change)
        sets.append(changed)
    return sets


def _edit(catalog: bytes, job_plists: list[bytes]) -> bytes | None:
    try:
        return add_bootstrap_jobs(catalog, *(plistlib.loads(data) for data in job_plists))
    except REFUSALS:
        return None


def edit_cases(jobs: list[bytes]) -> list[Case]:
    """Every catalogue with the project's jobs, and the first catalogue with every set of jobs."""
    catalogues = _catalogues() + _refused_catalogues()
    pairs = [(catalogue, jobs) for catalogue in catalogues]
    pairs += [(catalogues[1], job_plists) for job_plists in _job_sets(jobs)[1:]]
    return [(catalogue, job_plists, _edit(catalogue, job_plists)) for catalogue, job_plists in pairs]
