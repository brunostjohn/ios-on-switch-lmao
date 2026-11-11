"""logharvest.py over ndjson records shaped as `log show --style ndjson` prints the raw runtime's lines."""

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import logharvest

RB = '/System/Library/PrivateFrameworks/RenderBox.framework/default.metallib'
HEAD = 'samples=1 topo=0 ds=260/260 c0=80:15:0:1:0:0:1:0:0 c1=105:0:0:1:0:0:1:0:0 c2=80:0:0:1:0:0:1:0:0 '
COLOR = f'{HEAD} | v primitive__vertex  | f primitive_color_fragment #0:36:4501 | l RenderBox'
GRADIENT = f'{HEAD} | v primitive_gradient_vertex  | f primitive_gradient_fragment  | l RenderBox'
IMAGE = f'{HEAD} | v primitive_image_vertex #0:36:8700 | f primitive_image_fragment #0:36:8700 | l RenderBox'
CUBIC = ' | cubic s0:0000n t0:80'
CA = (
    'samples=1 topo=0 ds=0/0 c0=80:15:1:1:5:0:1:5:0 a0=31:0:1:48:1:1  | v fixed_vert_lph_spc '
    + ' '.join(f'fc_c{i}:49:01' for i in range(40))
    + ' | f fixed_frag_lph_cph '
    + ' '.join(f'fc_d{i}:49:00' for i in range(40))
)
CA_LABEL = 'com.apple.coreanimation.PBGRAXm_A3Xhfc'
CORPUS = (
    f'R ok=1 label=primitive_image samples=1 topo=0 ds=260/260 c0=80:15:0:1:0:0:1:0:0 c1=105:0:0:1:0:0:1:0:0'
    f' c2=80:0:0:1:0:0:1:0:0 | v primitive_image_vertex lib={RB} #0:36:8700'
    f' | f primitive_image_fragment lib={RB} #0:36:8700 | \n'
    f'R ok=1 label=- os=21H461 {HEAD.rstrip()} | v primitive_gradient_vertex lib={RB}  '
    f'| f primitive_gradient_fragment lib={RB}  | \n'
)
BOOT, OTHER = 'AAAAAAAA-0000-0000-0000-000000000001', 'BBBBBBBB-0000-0000-0000-000000000002'


def record(boot: str, process: str, message: str) -> dict:
    return {
        'eventType': 'logEvent',
        'bootUUID': boot,
        'processImagePath': f'/System/Library/CoreServices/{process}',
        'eventMessage': message,
    }


def made(boot: str, process: str, name: str, key: str, outcome: str = 'made') -> dict:
    message = (
        f'raw: a state the pack lacks {outcome} in 120 ms (UI) after 0 ms for a compile slot: {name} {key[:400]}'
    )
    return record(boot, process, message)


def parts(boot: str, process: str, name: str, label: str, key: str) -> list[dict]:
    text = f'label={label} {key}'
    chunks = [text[i : i + 700] for i in range(0, len(text), 700)]
    return [
        record(boot, process, f'raw: harvest {name} {i + 1}/{len(chunks)} {chunk}') for i, chunk in enumerate(chunks)
    ]


def variant(boot: str, process: str, state: str, name: str) -> dict:
    hashed = f'{logharvest.rawpack.packfile.fnv(state.encode()):016x}'
    message = (
        f'{process}[323] state.variant_unpacked: a packed state\'s variant the pack lacks: its draws are refused:'
        f'{name} of {hashed} {state}'
    )
    return record(boot, process, message[:250])


def boot_records() -> list[dict]:
    return [
        record(BOOT, 'CallHistorySyncHelper', 'OS version: 24B119'),
        made(BOOT, 'WidgetRenderer', '0000000000000001', COLOR),
        made(BOOT, 'iconservicesagent', '0000000000000002', GRADIENT),
        made(BOOT, 'iconservicesagent', '0000000000000003', IMAGE + CUBIC),
        made(BOOT, 'SwitchHWTest', '0000000000000004', COLOR.replace('4501', '4502')),
        made(BOOT, 'backboardd', '0000000000000005', CA),
        *parts(BOOT, 'backboardd', '0000000000000005', CA_LABEL, CA),
        made(BOOT, 'backboardd', '0000000000000006', CA.replace('fc_d1:', 'fc_e1:')),
        made(BOOT, 'cameracaptured', '0000000000000007', COLOR.replace('4501', '4503'), 'refused'),
        made(OTHER, 'WidgetRenderer', '0000000000000008', COLOR.replace('4501', '4504')),
    ]


class LogHarvestTest(unittest.TestCase):
    def run_tool(self, records: list[dict], *flags: str) -> tuple[list[str], str]:
        with tempfile.TemporaryDirectory() as scratch:
            corpus = Path(scratch) / 'corpus.log'
            corpus.write_text(CORPUS)
            ndjson = Path(scratch) / 'boot.ndjson'
            ndjson.write_text(''.join(json.dumps(r) + '\n' for r in records))
            logharvest.main(['--ndjson', str(ndjson), '--corpus', str(corpus), *flags])
            return corpus.read_text().splitlines()[2:], corpus.read_text()

    def test_each_made_state_becomes_a_tagged_line_that_gives_back_its_key(self):
        added, _ = self.run_tool(boot_records())
        keys = [logharvest.rawpack.state_key(line) for line in added]
        self.assertEqual(sorted(keys), sorted([COLOR, GRADIENT, IMAGE + CUBIC, CA]))
        self.assertTrue(all(' os=24B119 ' in line for line in added))

    def test_a_ca_state_carries_its_label_from_the_harvest_parts(self):
        added, _ = self.run_tool(boot_records())
        (line,) = [line for line in added if 'fixed_frag_lph_cph' in line]
        self.assertTrue(line.startswith(f'R ok=1 label={CA_LABEL} os=24B119 '))

    def test_a_framework_states_line_names_its_library_on_both_stages(self):
        added, _ = self.run_tool(boot_records())
        (line,) = [line for line in added if 'primitive_color_fragment' in line]
        self.assertEqual(line.count(f' lib={RB}'), 2)

    def test_the_same_key_tagged_with_another_build_is_added_for_this_one(self):
        added, _ = self.run_tool(boot_records())
        self.assertIn(GRADIENT, [logharvest.rawpack.state_key(line) for line in added])

    def test_an_untagged_corpus_line_counts_for_every_build(self):
        added, _ = self.run_tool(boot_records())
        self.assertNotIn(IMAGE, [logharvest.rawpack.state_key(line) for line in added])

    def test_a_boot_naming_no_build_is_harvested_only_under_build(self):
        added, _ = self.run_tool([record(BOOT, 'x', 'OS version: 24B119'), *boot_records()[-1:]])
        self.assertEqual(added, [])
        added, _ = self.run_tool(boot_records()[-1:], '--build', '24B119')
        self.assertEqual([logharvest.rawpack.state_key(line) for line in added], [COLOR.replace('4501', '4504')])

    def test_a_boot_naming_another_build_than_build_is_refused(self):
        with self.assertRaises(SystemExit):
            self.run_tool(boot_records(), '--build', '21H461')

    def test_a_variant_line_finds_its_state_by_hash(self):
        records = [record(BOOT, 'x', 'OS version: 24B119'), variant(BOOT, 'iconservicesagent', IMAGE, CUBIC)]
        added, _ = self.run_tool(records)
        self.assertEqual([logharvest.rawpack.state_key(line) for line in added], [IMAGE + CUBIC])

    def test_a_dry_run_leaves_the_corpus_as_it_was(self):
        _, text = self.run_tool(boot_records(), '--dry-run')
        self.assertEqual(text, CORPUS)

    def test_a_second_run_adds_nothing(self):
        with tempfile.TemporaryDirectory() as scratch:
            corpus = Path(scratch) / 'corpus.log'
            corpus.write_text(CORPUS)
            ndjson = Path(scratch) / 'boot.ndjson'
            ndjson.write_text(''.join(json.dumps(r) + '\n' for r in boot_records()))
            for _ in range(2):
                logharvest.main(['--ndjson', str(ndjson), '--corpus', str(corpus)])
            self.assertEqual(len(corpus.read_text().splitlines()), 2 + 4)

    def test_left_out_states_are_counted_by_reason(self):
        boots = logharvest.read_boots(boot_records())
        _, report = logharvest.harvest(boots, CORPUS.splitlines(), None, Path('/nonexistent'))
        self.assertEqual(report.left['SwitchHWTest', 'our own app'], 1)
        self.assertEqual(report.left['cameracaptured', 'refused on the device'], 1)
        self.assertEqual(report.left['backboardd', 'key cut at 400 characters, no whole parts'], 1)


if __name__ == '__main__':
    unittest.main()
