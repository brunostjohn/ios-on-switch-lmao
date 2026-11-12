"""Per-draw goldens of an nvdec log as raw/pack/eqcheck.py sees it, and a log held to them as eqcheck holds two.

usage: draw_goldens.py write <log> <golden>
       draw_goldens.py check <log> <golden>

A golden line per item of each frame:
  C <frame> <clears>                    the frame's clears in order (eqcheck.split_depth_stencil), sha256/16 each
  E <frame> <kind> <i> <digest> <bars>  a draw's or dispatch's fields but its barriers (its live registers less the
                                        pools' limits as one field), sha256/16 of all, and its barriers
  F <frame> <kind> <i> <field> <hash>   each field's own hash, for a fixed sample of the events
A log passes where eqcheck would pass it against the golden's: the same fields, every barrier the golden requires
covered (one the raw path marks ELIDED excused), the same clears but the first frame's that only the golden has.
"""

import hashlib
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'raw/pack'))
import eqcheck

SAMPLED = 8  # one event in SAMPLED, by the hash of its place, keeps its fields' hashes


def digest(text: str) -> str:
    return hashlib.sha256(text.encode()).hexdigest()[:32]


def field_hashes(event: dict) -> tuple[dict[str, str], str]:
    hashes, barriers = {}, '-'
    for name, value in eqcheck.fields(event).items():
        if name == 'barriers':
            barriers = ','.join(sorted(value)) or '-'
        elif name == 'R':
            hashes[name] = digest(
                ' '.join(f'{m}={v}' for m, v in sorted(value.items()) if m not in eqcheck.POOL_LIMITS)
            )
        elif name != 'regs':
            hashes[name] = digest(str(value))
    return hashes, barriers


def golden_of(log: str) -> list[list[str]]:
    rows = []
    for fi, events in enumerate(eqcheck.rename_all(eqcheck.frames(log))):
        clears = [eqcheck.fields(e)['clear'] for e in events if e['head'].startswith('CLEAR')]
        rows.append(['C', str(fi), ','.join(digest(c) for c in eqcheck.split_depth_stencil(clears)) or '-'])
        for kind in ('DRAW', 'DISPATCH'):
            for i, event in enumerate(e for e in events if e['head'].startswith(kind)):
                place = [str(fi), kind.lower(), str(i)]
                hashes, barriers = field_hashes(event)
                rows.append(['E', *place, digest(' '.join(f'{k}={v}' for k, v in sorted(hashes.items()))), barriers])
                if int(digest(' '.join(place)), 16) % SAMPLED == 0:
                    rows += [['F', *place, name, h] for name, h in sorted(hashes.items())]
    return rows


def read(golden: str) -> list[list[str]]:
    return [line.split('\t') for line in Path(golden).read_text().splitlines()]


def clears_moved(fi: str, want: str, got: str) -> list[str]:
    nvk = [c for c in want.split(',') if c != '-']
    raw = [c for c in got.split(',') if c != '-']
    if fi == '0':
        nvk = [c for c in nvk if c in raw]  # textures m2v initialised in the first frame
    return [] if nvk == raw else [f'frame {fi}: {len(raw)} clears, the golden {len(nvk)} or another order']


def barriers_moved(where: str, want: str, got: str) -> list[str]:
    need = set() if want == '-' else set(want.split(','))
    made = set() if got == '-' else set(got.split(','))
    if eqcheck.compare_barriers(eqcheck.Tally(0), where, need, made):
        return []
    return [f'{where}: barriers {sorted(made)}, the golden {sorted(need)}']


def differences(want_rows: list[list[str]], got_rows: list[list[str]]) -> list[str]:
    got = {tuple(r[1:-2] if r[0] == 'E' else r[1:-1]): r for r in got_rows if r[0] in 'CE'}
    got_fields = {tuple(r[1:5]): r[5] for r in got_rows if r[0] == 'F'}
    moved = []
    for row in want_rows:
        if row[0] == 'C':
            moved += clears_moved(row[1], row[2], got.get((row[1],), ['C', row[1], '-'])[2])
        elif row[0] == 'E':
            have = got.pop(tuple(row[1:4]), None)
            where = ' '.join(row[1:4])
            if have is None:
                moved.append(f'{where}: not in the log')
                continue
            moved += barriers_moved(where, row[5], have[5])
            if have[4] != row[4]:
                moved.append(f'{where}: moved')
        elif got_fields.get(tuple(row[1:5])) != row[5]:
            moved.append(f'{" ".join(row[1:4])}: {row[4]} moved')
    moved += [f'{" ".join(k)}: only in the log' for k, r in got.items() if r[0] == 'E']
    return moved


def main() -> int:
    mode, log, golden = sys.argv[1:4]
    if mode == 'write':
        rows = golden_of(log)
        Path(golden).write_text(''.join('\t'.join(r) + '\n' for r in rows))
        print(f'{sum(r[0] == "E" for r in rows)} events -> {golden}')
        return 0
    want = read(golden)
    moved = differences(want, golden_of(log))
    for line in moved[:40]:
        print(line)
    print(f'{sum(r[0] == "E" for r in want)} golden events: {len(moved)} differences')
    return 1 if moved else 0


if __name__ == '__main__':
    sys.exit(main())
