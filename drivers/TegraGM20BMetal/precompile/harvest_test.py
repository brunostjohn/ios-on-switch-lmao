"""harvest.py finds a system process's m2v cache by its container's identifier, whatever UUID the install gave it."""

import hashlib
import json
import plistlib
import struct
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from drivers.TegraGM20BMetal.precompile import harvest


def container(volume: Path, uuid: str, identifier: str) -> Path:
    root = volume / 'containers/Data/System' / uuid
    (root / 'Library/Caches/m2v').mkdir(parents=True)
    with (root / harvest.CONTAINER_METADATA).open('wb') as f:
        plistlib.dump({'MCMMetadataIdentifier': identifier}, f)
    (root / 'Library').chmod(0)
    return root


class Lines(unittest.TestCase):
    def test_a_render_line_keeps_its_build_and_its_variants_binds(self):
        line = (
            'R ok=1 label=primitive_image os=24B119 samples=1 topo=0 ds=260/260 t=12 c0=80:0:0:1:0:0:1:0:0'
            ' | v primitive_image_vertex #0:36:4700 | f primitive_image_fragment #0:36:4700'
            ' | cubic s0:0000n t0:80 | ms=4'
        )
        kept, key = harvest.normal(line)
        self.assertEqual(
            kept,
            'R ok=1 label=primitive_image os=24B119 samples=1 topo=0 ds=260/260 c0=80:0:0:1:0:0:1:0:0'
            ' | v primitive_image_vertex #0:36:4700 | f primitive_image_fragment #0:36:4700 | cubic s0:0000n t0:80 | ',
        )
        self.assertNotEqual(key, harvest.normal(line.replace(' os=24B119', ' os=21H461'))[1])


class Caches(unittest.TestCase):
    def test_a_closed_system_container_is_opened_by_its_identifier(self):
        with tempfile.TemporaryDirectory() as d:
            volume = Path(d)
            backboardd = container(volume, 'CE929FE0-E622-45FA-8423-A8020D7412A1', 'com.apple.backboardd')
            other = container(volume, '00000000-0000-0000-0000-000000000000', 'com.apple.locationd')
            try:
                found, skipped = harvest.caches(volume, set(harvest.SYSTEM_CACHES), False, print)
                self.assertEqual([m2v for _, m2v, _ in found], [backboardd / 'Library/Caches/m2v'])
                self.assertEqual(skipped, [other.name])
                found, skipped = harvest.caches(volume, {other.name}, False, print, explicit=True)
                self.assertEqual([m2v for _, m2v, _ in found], [other / 'Library/Caches/m2v'])
                self.assertEqual(skipped, [backboardd.name])
            finally:
                for root in (backboardd, other):
                    (root / 'Library').chmod(0o755)


class Replies(unittest.TestCase):
    def test_harvest_keeps_provenance_for_already_retained_reply_and_pack(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, store = root / 'cache/msl', root / 'staged/msl'
            source.mkdir(parents=True)
            request, uuid = b'kernel source\0', bytes(range(16))
            key = hashlib.sha256(uuid + struct.pack('<I', 13) + request).hexdigest()
            output = b'MTLB fixture'
            reply = b'NSC1' + struct.pack('<IQ', 0, len(output)) + hashlib.sha256(output).digest() + output
            (source / (key + '.reply')).write_bytes(reply)
            self.assertEqual(harvest.replies(source.parent, store), 1)
            (source / (key + '.request')).write_bytes(request)
            provenance = source / 'provenance/24B119' / (key + '.json')
            provenance.parent.mkdir(parents=True)
            provenance.write_text(
                json.dumps(
                    {
                        'schema': 1,
                        'reply': key + '.reply',
                        'compilerUUID': uuid.hex(),
                        'requestType': 13,
                        'requestBytes': len(request),
                        'requestSHA256': hashlib.sha256(request).hexdigest(),
                        'replySHA256': hashlib.sha256(reply).hexdigest(),
                        'metallibSHA256': hashlib.sha256(output).hexdigest(),
                        'osBuild': '24B119',
                        'targetBuild': '24B119',
                    }
                )
            )
            self.assertEqual(harvest.replies(source.parent, store), 0)
            self.assertEqual((store / provenance.relative_to(source)).read_bytes(), provenance.read_bytes())
            self.assertEqual((store / (key + '.request')).read_bytes(), request)
            corpus = root / 'corpus.log'
            corpus.write_text('')
            with (
                mock.patch.object(harvest, 'DEVICE', root / 'packed'),
                mock.patch.object(harvest, 'embedded_libraries', return_value=set()),
            ):
                harvest.pack(store.parent, corpus)
            archive = next((root / 'packed').glob('*.tar'))
            with tarfile.open(archive) as packed:
                self.assertEqual(
                    set(packed.getnames()),
                    {
                        'msl/' + key + '.reply',
                        'msl/' + key + '.request',
                        'msl/provenance/24B119/' + key + '.json',
                    },
                )
                self.assertEqual(packed.extractfile('msl/' + key + '.request').read(), request)

    def test_harvest_refuses_conflicting_valid_reply(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, target = root / 'cache/msl', root / 'target'
            for folder, output in [(source, b'first'), (target, b'second')]:
                folder.mkdir(parents=True)
                data = b'NSC1' + struct.pack('<IQ', 0, len(output)) + hashlib.sha256(output).digest() + output
                (folder / ('a' * 64 + '.reply')).write_bytes(data)
            with self.assertRaisesRegex(ValueError, 'conflicting'):
                harvest.replies(source.parent, target)


if __name__ == '__main__':
    unittest.main()
