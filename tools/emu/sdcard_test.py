import json
import struct
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from tools.emu import sdcard


class FilesCardTest(unittest.TestCase):
    def test_only_hybrid_type_changes_between_filesystems(self):
        plan = sdcard.layout(sdcard.NO_IMAGE_SECTORS)
        fat, fat_tail = sdcard.tables(plan)
        exfat, exfat_tail = sdcard.tables(plan, 'exfat')
        self.assertEqual(exfat[450], 7)
        self.assertEqual(fat[450], 12)
        self.assertEqual(fat[:450] + fat[451:], exfat[:450] + exfat[451:])
        self.assertEqual(fat_tail, exfat_tail)
        self.assertEqual(struct.unpack_from('<II', exfat, 454), (plan['fat'][0], sdcard.FAT_SECTORS))

    def test_exfat_seed_uses_run_head_and_rejects_unknown_filesystem(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            entry = root / 'shared-base'
            entry.mkdir()
            (entry / 'layout.json').write_text(json.dumps({'plan': sdcard.layout(1), 'ios': [[1, None, 0]]}))
            bootargs = root / 'bootargs.txt'
            bootargs.write_text('-v\n')
            files = {'switchroot/ios/bootargs.txt': bootargs}

            def image(run: Path, _files: dict[str, Path]) -> Path:
                target = run / 'card-fat.img'
                target.write_bytes(b'original volume')
                return target

            with (
                patch.object(sdcard, 'base', return_value=entry),
                patch.object(sdcard, '_make_exfat', side_effect=image),
            ):
                card = sdcard.card_for_run(None, root / 'run', files, 'exfat')
                descriptor = card.read_text()
                self.assertIn('FLAT "head.img"', descriptor)
                self.assertIn('FLAT "card-fat.img"', descriptor)
                self.assertIn(str(entry / 'tail.img'), descriptor)
            with self.assertRaises(ValueError):
                sdcard.card_for_run(None, root / 'bad', files, 'ntfs')

    def test_raw_container_defaults_to_offset_zero_without_state_json(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            image = root / 'raw.apfs'
            header = bytearray(4096)
            struct.pack_into('<4sIQ', header, 32, b'NXSB', 4096, 1024)
            image.write_bytes(header)

            self.assertEqual(sdcard.raw_container(image), (0, 4096 * 1024 // sdcard.SECTOR))

            captured = {}

            def fake_entry(_root: Path, key: str, _builder: object) -> Path:
                captured['key'] = key
                return root / 'cached'

            with patch.object(sdcard.cache, 'entry', side_effect=fake_entry):
                self.assertEqual(sdcard.base(image), root / 'cached')
            self.assertIn('key', captured)

            offset_image = root / 'staged.img'
            offset_bytes = 2048 * sdcard.SECTOR
            with offset_image.open('wb') as f:
                f.seek(offset_bytes + 32)
                f.write(struct.pack('<4sIQ', b'NXSB', 4096, 512))
            offset_image.with_name('state.json').write_text(json.dumps({'offset': offset_bytes}))
            self.assertEqual(sdcard.raw_container(offset_image), (2048, 4096 * 512 // sdcard.SECTOR))

            bad_image = root / 'bad.apfs'
            bad_image.write_bytes(b'\0' * 64)
            with self.assertRaises(SystemExit):
                sdcard.raw_container(bad_image)


if __name__ == '__main__':
    unittest.main()
