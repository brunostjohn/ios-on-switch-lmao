"""The GPU stager on a plain directory standing in for the Data volume.

It copies only what changed, reads nothing when the card's list matches the build, and a cut-short stage is finished by
the next one.
"""

import gzip
import importlib.util
import tempfile
import unittest
from pathlib import Path
from unittest import mock

SPEC = importlib.util.spec_from_file_location(
    'stage', Path(__file__).resolve().parents[1] / 'stage-tegra-gm20b-metal.py'
)
stage = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(stage)

SELF = '_main/gpu/bundle.stage'
SEED = 'ngpu/addons/TegraGM20BMetal.seed/shared'


class Card:
    """A plain directory standing in for the Data volume, and a //gpu:bundle manifest to stage onto it."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.runfiles = self.root / 'runfiles'
        self.data = self.root / 'data'
        (self.data / 'mobile').mkdir(parents=True)
        self.files = {
            stage.BUNDLE + '/TegraGM20BMetal': b'plugin',
            stage.BUNDLE + '/Info.plist': b'plist',
            SEED + '/index': b'index',
            SEED + '/packs/000': b'pack zero',
            SEED + '/packs/001': b'pack one',
            stage.RAW + '/pack.bin': b'raw',
        }
        self.kext = ''

    def tearDown(self):
        self.tmp.cleanup()

    def manifest(self) -> Path:
        lines = []
        for path, data in sorted(self.files.items()):
            source = self.runfiles / '_main/out' / path
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_bytes(data)
            mode = '755' if path.endswith('/TegraGM20BMetal') else '644'
            digest = stage.hashlib.sha256(data).hexdigest()
            lines.append(f'{path}\t{digest}\t{len(data)}\t{mode}\t_main/out/{path}')
        manifest = self.runfiles / SELF
        manifest.parent.mkdir(parents=True, exist_ok=True)
        kext = stage.MANIFEST_KEXT + self.kext + '\n' if self.kext else ''
        manifest.write_text(stage.MANIFEST_HEADER + SELF + '\n' + kext + ''.join(line + '\n' for line in lines))
        return manifest

    def sync(self) -> str:
        manifest = self.manifest()
        return stage.sync(self.data, stage.from_manifest(manifest), lambda _: None, stage.manifest_kext(manifest))

    def card(self) -> dict[str, bytes]:
        top = self.data / 'ngpu/addons'
        return {p.relative_to(self.data).as_posix(): p.read_bytes() for p in top.rglob('*') if p.is_file()}


class StageTest(Card, unittest.TestCase):
    def test_first_stage_replaces_the_directories_and_copies_everything(self):
        (self.data / SEED / 'v1/aa').mkdir(parents=True)
        (self.data / SEED / 'v1/aa/old').write_bytes(b'one file per entry')
        self.assertTrue(self.sync().startswith('6 copied'))
        self.assertEqual(self.card(), self.files)
        self.assertEqual((self.data / stage.BUNDLE / 'TegraGM20BMetal').stat().st_mode & 0o777, 0o755)
        self.assertFalse((self.data / stage.PENDING).exists())

    def test_the_folders_from_before_the_rename_go_where_the_new_ones_are_staged(self):
        for old in stage.RENAMED_FROM:
            (self.data / old / 'native/raw-0123456789abcdef').mkdir(parents=True)
            (self.data / old / 'native/raw-0123456789abcdef/index').write_bytes(b'old')
        done = self.sync()
        self.assertEqual(self.card(), self.files)
        for old in stage.RENAMED_FROM:
            self.assertIn(f'removed {old}', done)
            self.assertFalse((self.data / old).exists())

    def test_an_unchanged_build_reads_nothing_but_the_lists_first_line(self):
        self.sync()
        manifest = self.manifest()
        for source in (self.runfiles / '_main/out').rglob('*'):
            if source.is_file():
                source.unlink()
        self.assertTrue(stage.sync(self.data, stage.from_manifest(manifest), lambda _: None).startswith('unchanged'))

    def test_a_change_copies_that_file_and_a_dropped_file_goes(self):
        self.sync()
        self.files[SEED + '/packs/001'] = b'pack one, recompiled'
        del self.files[stage.RAW + '/pack.bin']
        self.assertTrue(self.sync().startswith('1 copied (0 MiB), 1 removed, 4 unchanged'))
        self.assertEqual(self.card(), self.files)
        self.assertFalse((self.data / stage.RAW).exists(), 'an emptied directory goes too')

    def test_a_cut_short_stage_is_finished_by_the_next(self):
        self.sync()
        (self.data / SEED / 'packs/000').write_bytes(b'half wri')
        (self.data / stage.PENDING).write_text(SEED + '/packs/000\n')
        self.assertTrue(self.sync().startswith('1 copied'))
        self.assertEqual(self.card(), self.files)

    def test_the_list_names_what_is_staged(self):
        self.sync()
        with gzip.open(self.data / stage.STATE, 'rt') as f:
            lines = f.read().splitlines()
        self.assertEqual(lines[0], '# ' + stage.digest(stage.from_manifest(self.manifest())))
        self.assertEqual(sorted(line.split('\t')[0] for line in lines[1:]), sorted(self.files))

    def test_a_directory_build_stages_as_its_manifest_does(self):
        build = self.root / 'base'
        for path, data in self.files.items():
            (build / path).parent.mkdir(parents=True, exist_ok=True)
            (build / path).write_bytes(data)
        (build / 'TegraGM20BMetal.trustcache').write_bytes(b'trust')
        self.assertEqual(stage.digest(stage.from_directory(build)), stage.digest(stage.from_manifest(self.manifest())))


class IconCacheRecordTest(Card, unittest.TestCase):
    """The stager names what it staged for the kernel's icon cache drop (kernel/switch/boot/icon_cache.c)."""

    def record(self) -> str:
        return (self.data / 'Switch/icon-cache-gpu').read_text()

    def test_a_stage_names_the_lists_digest(self):
        self.sync()
        with gzip.open(self.data / stage.STATE, 'rt') as handle:
            first = handle.readline().strip()
        self.assertEqual(self.record(), first[2:] + '\n')

    def test_an_unchanged_stage_leaves_the_record_as_it_is_and_names_a_card_that_lacked_it(self):
        self.sync()
        record = self.data / 'Switch/icon-cache-gpu'
        before = record.stat().st_mtime_ns
        self.sync()
        self.assertEqual(record.stat().st_mtime_ns, before)
        record.unlink()
        self.assertTrue(self.sync().startswith('unchanged'))
        self.assertTrue(record.is_file())

    def test_a_new_build_is_named(self):
        self.sync()
        first = self.record()
        self.files[stage.BUNDLE + '/Info.plist'] = b'another plist'
        self.sync()
        self.assertNotEqual(self.record(), first)

    def test_a_kext_only_change_renames_the_build_and_copies_nothing(self):
        self.kext = 'a' * 64
        self.sync()
        first = self.record()
        with gzip.open(self.data / stage.STATE, 'rt') as handle:
            self.assertNotEqual(first, handle.readline().strip()[2:] + '\n')
        self.kext = 'b' * 64
        self.assertTrue(self.sync().startswith('unchanged'))
        self.assertNotEqual(self.record(), first)
        self.assertNotEqual(self.record()[:12], first[:12])  # the kernel keys its keep on the first 12 characters

    def test_a_manifest_without_a_kext_line_names_the_list_alone(self):
        self.sync()
        with gzip.open(self.data / stage.STATE, 'rt') as handle:
            self.assertEqual(self.record(), handle.readline().strip()[2:] + '\n')
        self.assertEqual(stage.manifest_kext(self.manifest()), '')

    def test_a_removed_build_is_named_as_none(self):
        self.sync()
        with (
            mock.patch.object(stage, '_check_volume'),
            mock.patch.object(stage.sys, 'argv', ['stage', str(self.data), '--remove']),
        ):
            stage.main()
        self.assertEqual(self.record(), '\n')

    def test_a_card_with_only_the_former_folder_gets_the_record_there(self):
        (self.data / 'SwitchOld/overlay').mkdir(parents=True)
        self.sync()
        self.assertTrue((self.data / 'SwitchOld/icon-cache-gpu').is_file())
        self.assertFalse((self.data / 'Switch').exists())


if __name__ == '__main__':
    unittest.main()
