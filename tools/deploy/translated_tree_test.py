"""translated_tree's check of a card's translated System files against this build's os.trustcache, on plain folders."""

import json
import tempfile
import unittest
from pathlib import Path

from tools.deploy import translated_tree
from tools.install import card as cards
from tools.trustcache import trustcache

PINNED = Path(__file__).parents[2] / 'ipsw/pretranslated/24B119/os.trustcache'
# locationd's translation before and after 15032ab57 kept capability bit 4 (edits_24B119.json).
LOCATIOND_BEFORE = 'ac43b78b6ebf882d05f51c6edf50289ec42b6b64'
LOCATIOND_AFTER = '020e95fca17c66f80bc7d1d0e62860ca8b3cf097'
OTHER = 'aa' * 20


def manifest(locationd: str) -> dict:
    return {
        'files': [
            {'path': '/usr/libexec/locationd', 'trust': True, 'mode': '0755', 'cdhash': [locationd]},
            {'path': '/usr/libexec/notrust', 'trust': False, 'cdhash': ['bb' * 20]},
        ]
    }


class TranslatedTree(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.trust = self.root / 'os.trustcache'
        self.trust.write_bytes(trustcache.trust_module([bytes.fromhex(LOCATIOND_AFTER), bytes.fromhex(OTHER)]))

    def tearDown(self):
        self.tmp.cleanup()

    def card(self, folder: str, contents: dict) -> Path:
        data = self.root / 'data'
        (data / folder / 'overlay').mkdir(parents=True)
        (data / folder / translated_tree.MANIFEST).write_text(json.dumps(contents))
        return data

    def test_a_card_this_build_trusts_deploys(self):
        translated_tree.check(self.card('Switch', manifest(LOCATIOND_AFTER)), self.trust)

    def test_a_translated_file_the_build_no_longer_trusts_refuses_naming_it_and_the_install(self):
        with self.assertRaises(cards.RefusedError) as refused:
            translated_tree.check(self.card('Switch', manifest(LOCATIOND_BEFORE)), self.trust)
        self.assertIn('/usr/libexec/locationd', str(refused.exception))
        self.assertIn('//install:sd', str(refused.exception))

    def test_the_folder_under_its_former_name_is_read(self):
        with self.assertRaises(cards.RefusedError):
            translated_tree.check(self.card('IOSOld', manifest(LOCATIOND_BEFORE)), self.trust)

    def test_a_card_without_the_manifest_refuses(self):
        (self.root / 'data/Switch/overlay').mkdir(parents=True)
        with self.assertRaises(cards.RefusedError) as refused:
            translated_tree.check(self.root / 'data', self.trust)
        self.assertIn('//install:sd', str(refused.exception))

    def test_the_pinned_trust_cache_refuses_the_locationd_cards_carry_from_before_the_edit(self):
        pinned = PINNED.read_bytes()
        self.assertEqual(translated_tree.untrusted(manifest(LOCATIOND_BEFORE), pinned), ['/usr/libexec/locationd'])
        self.assertEqual(translated_tree.untrusted(manifest(LOCATIOND_AFTER), pinned), [])


if __name__ == '__main__':
    unittest.main()
