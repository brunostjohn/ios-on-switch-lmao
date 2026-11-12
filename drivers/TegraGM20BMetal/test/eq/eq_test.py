"""raw's frame against NVK's: rawframe over the eq capture, its log held to the NVK side's per-draw goldens.

usage: eq_test.py <rawframe> <pack.bin> <kind> <testdata dir> [--qc27]

The frame is the one the NVK side was recorded with (nvkframe with the same corpus,
arguments and FRAME_* settings), its programs named by NVK's own programs.bin, so a program raw makes byte for byte as
NVK made it has NVK's name in both logs.
"""

import gzip
import os
import subprocess
import sys
from pathlib import Path

import draw_goldens


def unzip(source: Path, target: Path) -> Path:
    target.write_bytes(gzip.decompress(source.read_bytes()))
    return target


def main() -> int:
    rawframe, pack, kind, testdata, *flags = sys.argv[1:]
    data, tmp = Path(testdata), Path(os.environ.get('TEST_TMPDIR', '/tmp'))
    kept = Path(os.environ.get('TEST_UNDECLARED_OUTPUTS_DIR', tmp))
    corpus = unzip(data / f'frame_{kind}.corpus.gz', tmp / 'frame.corpus')
    env = dict(
        os.environ,
        RAWCAP_LOG=str(kept / 'raw.log'),
        RAWCAP_PROGRAMS=str(unzip(data / f'programs_{kind}.bin.gz', tmp / 'programs.bin')),
        NVDEC_REGS='1',
        FRAME_ALL_STATES='1',
        FRAME_STATES='2000',
    )
    if '--qc27' in flags:
        env['FRAME_QC27'] = '1'
    run = subprocess.run(
        [rawframe, pack, str(corpus), '1', '8', '2000'], env=env, capture_output=True, text=True, check=False
    )
    if run.returncode:
        print(f'rawframe exited {run.returncode}:\n{run.stderr[-3000:]}')
        return 1
    want = draw_goldens.read(str(data / f'draws_{kind}.tsv'))
    moved = draw_goldens.differences(want, draw_goldens.golden_of(str(kept / 'raw.log')))
    for line in moved[:40]:
        print(line)
    print(f'{sum(r[0] == "E" for r in want)} NVK events: {len(moved)} differences')
    return 1 if moved else 0


if __name__ == '__main__':
    sys.exit(main())
