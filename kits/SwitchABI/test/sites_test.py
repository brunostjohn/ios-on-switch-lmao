"""sites.py: a declared site is written only when every original word matches, and only for its own kind."""

import json
import tempfile
import unittest
from pathlib import Path

from kits.SwitchABI import sites

TABLES = Path(__file__).resolve().parents[1] / 'kinds'


def table(kind: str, rows: list[tuple[str, str, str]]) -> Path:
    path = Path(tempfile.mkdtemp()) / 'com.example.kext.json'
    path.write_text(
        json.dumps(
            {
                'kind': kind,
                'bundle': 'com.example.kext',
                'sites': [{'at': a, 'from': f, 'to': t, 'why': 'test'} for a, f, t in rows],
            }
        )
    )
    return path


class SitesTest(unittest.TestCase):
    def setUp(self):
        self.words = {0x1000: 0x71000A9F, 0x1004: 0x52A00808, 0x1008: 0xD503201F}

    def test_patched_at_exactly_the_declared_sites(self):
        before = dict(self.words)
        rows = [('0x1000', '71000a9f', '7100029f'), ('0x1004', '52a00808', '52800008')]
        found = sites.load(table('24B119', rows), '24B119')
        sites.apply(found, self.words.__getitem__, self.words.__setitem__)
        changed = {a for a in before if before[a] != self.words[a]}
        self.assertEqual(changed, {0x1000, 0x1004})
        self.assertEqual(self.words[0x1000], 0x7100029F)

    def test_a_word_that_differs_fails_before_anything_is_written(self):
        before = dict(self.words)
        rows = [('0x1000', '71000a9f', '7100029f'), ('0x1004', '52a00809', '52800009')]
        found = sites.load(table('24B119', rows), '24B119')
        with self.assertRaises(SystemExit) as raised:
            sites.apply(found, self.words.__getitem__, self.words.__setitem__)
        self.assertIn('0x1004', str(raised.exception))
        self.assertEqual(self.words, before)

    def test_another_kinds_table_is_refused(self):
        with self.assertRaises(SystemExit):
            sites.load(table('21H461', [('0x1000', '71000a9f', '7100029f')]), '24B119')

    def test_a_site_declared_twice_is_refused(self):
        rows = [('0x1000', '71000a9f', '7100029f')]
        with self.assertRaises(SystemExit):
            sites.load_all([table('24B119', rows), table('24B119', rows)], '24B119')

    def test_the_kinds_tables_load(self):
        for kind in ('24B119', '21H461'):
            for path in sorted((TABLES / kind / 'sites').glob('*.json')):
                found = sites.load(path, kind)
                self.assertTrue(found, path)
                self.assertTrue(all(s.original != s.replacement for s in found), path)


if __name__ == '__main__':
    unittest.main()
