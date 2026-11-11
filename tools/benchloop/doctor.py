"""First contact: the Switch, a bench folder both sides reach, and the calibration batch through it.

The calibration batch times empty passes, runs the app's known-answer program and the host's encoding of the same
program, and dispatches an EXIT kernel. The folder that worked is remembered for every later run.
"""

import tempfile
from pathlib import Path

from tools.benchloop import golden, known, loop, protocol, session, transport

FLOAT = {'format': 'rgba32float', 'width': 16, 'height': 16}
KNOWN_TEXEL = known.KNOWN_DATA_BYTES.hex()


def calibration() -> list[dict]:
    return [
        {'id': 'noop', 'kind': 'noop', 'target': FLOAT, 'draws': [0], 'repeats': 5, 'warmup': 1, 'readback': 'none'},
        {'id': 'known', 'kind': 'known', 'target': FLOAT, 'readback': 'summary'},
        {
            'id': 'host-known',
            'kind': 'fragment',
            'target': FLOAT,
            'program': golden.program(),
            'draws': [1, 2, 4, 8],
            'repeats': 5,
            'warmup': 2,
        },
        {
            'id': 'exit-kernel',
            'kind': 'compute',
            'target': FLOAT,
            'compute': {
                'code': protocol.b64(known.KNOWN_KERNEL),
                'gprs': 1,
                'threads': [32, 1, 1],
                'groups': [8, 1, 1],
            },
            'draws': [1, 4],
            'repeats': 3,
            'readback': 'none',
        },
    ]


def judge(results: dict[str, dict]) -> tuple[bool, list[str]]:
    lines, ok = [], True
    for e in calibration():
        r = results.get(e['id'])
        if not r:
            lines.append(f'{e["id"]}: no result')
            ok = False
            continue
        gpu = [ns for run in r.get('runs', []) for ns in run.get('gpu_ns', [])]
        good = r['status'] == 'ok' and bool(gpu) and all(ns > 0 for ns in gpu)
        detail = f'{r["status"]} gpu_ns={sorted(gpu)[len(gpu) // 2] if gpu else "-"}'
        if e.get('readback', 'summary') != 'none':
            distinct = (r.get('texels') or {}).get('distinct', [])
            right = len(distinct) == 1 and distinct[0]['value'] == KNOWN_TEXEL
            good = good and right
            detail += f' texels={"all the known answer" if right else distinct[:2]}'
        if r.get('why'):
            detail += f' why={r["why"]}'
        ok = ok and good
        lines.append(f'{e["id"]}: {"PASS" if good else "FAIL"} {detail}')
    return ok, lines


def roots(udid: str | None) -> list[transport.Afc]:
    found = []
    for bundle in transport.BUNDLES:
        afc = transport.Afc(udid=udid, documents=bundle)
        if afc.ls('/Documents') is not None:
            found.append(afc)
    media = transport.Afc(udid=udid)
    if media.ls('/') is not None:
        found.append(media)
    return found


def prove(s: session.Session, into: Path) -> tuple[bool, list[str]]:
    outcome = s.run(protocol.batch('calibration', calibration()), into)
    ok, lines = judge(outcome.results)
    if not outcome.complete:
        lines.append(f'no done.json after {outcome.seconds:.0f} s (is the Bench screen open?)')
    return ok and outcome.complete, lines


def doctor(udid: str | None, into: Path) -> bool:
    udids = transport.devices()
    if not udids:
        print('no device: plug the Switch in while iOS runs, unlock it and trust this Mac')
        return False
    udid = udid or udids[0]
    print(f'device {udid}: iOS {session.paired(udid)}')
    candidates = roots(udid)
    if not candidates:
        print('neither house_arrest nor AFC answers: is HW Test installed, the device paired?')
        return False
    for afc in candidates:
        afc.mkdir(f'{afc.root}/inbox')
        afc.mkdir(f'{afc.root}/outbox')
    for afc in candidates:
        if 'status.json' not in (afc.ls(afc.root) or []):
            print(f'{afc.name}: the app is not watching it yet (open HW Test, tap Bench, or close and reopen Bench)')
            continue
        stream = loop.LogStream(udid)
        try:
            ok, lines = prove(session.Session(afc, stream), into)
        finally:
            stream.close()
        print('\n'.join('  ' + line for line in lines))
        if ok:
            session.remember(afc)
            print(f'loop proven through {afc.name}; later runs use it')
            return True
    return False


def proven_on_fake() -> tuple[bool, list[str]]:
    with tempfile.TemporaryDirectory() as scratch:
        s = session.fake(Path(scratch) / 'device')
        return prove(s, Path(scratch) / 'results')
