import hashlib
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools.gpu import stage_manifest


class StageManifestTest(unittest.TestCase):
    def test_packed_native_records_and_analysis_reach_the_runtime_lookup_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            records = 'raw-0123456789abcdef/'
            analysis = 'native-analysis/'
            inputs = {
                'base/ngpu/addons/TegraGM20BMetal.bundle/TegraGM20BMetal': b'plugin',
                'seed/SEED': b'compatible seed',
                'seed/MANIFEST.json': b'{}',
                'pack': b'compatibility records',
                'raw/pack.bin': b'raw pipeline pack',
                'native/' + records + 'index': b'native record index',
                'native/' + records + 'packs/000': b'native records',
                'native/' + analysis + 'index': b'native analysis index',
                'native/' + analysis + 'packs/000': b'native analyses',
            }
            host = {'native/manifest.json': b'native manifest', 'native/coverage.json': b'{"complete":false}'}
            for name, data in (inputs | host).items():
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
            output = root / 'bundle.stage'
            args = ['stage_manifest.py', '--self', 'bundle.stage', '--out', str(output)]
            for name in ('base', 'seed', 'raw', 'native'):
                args += ['--' + name, str(root / name), '--' + name + '-rlocation', name]
            args += ['--pack', str(root / 'pack'), '--pack-rlocation', 'pack']
            with mock.patch.object(sys, 'argv', args):
                stage_manifest.main()
            lines = output.read_text().splitlines()
            self.assertEqual(lines[0], stage_manifest.HEADER + 'bundle.stage')
            files = {parts[0]: parts[1:] for parts in (line.split('\t') for line in lines[1:])}
            self.assertEqual(len(files), len(inputs))
            expected = {
                stage_manifest.RAW + '/native/' + records + 'index': 'native/' + records + 'index',
                stage_manifest.RAW + '/native/' + records + 'packs/000': 'native/' + records + 'packs/000',
                stage_manifest.SEED + '/' + analysis + 'index': 'native/' + analysis + 'index',
                stage_manifest.SEED + '/' + analysis + 'packs/000': 'native/' + analysis + 'packs/000',
                stage_manifest.RAW + '/pack.bin': 'raw/pack.bin',
                stage_manifest.SEED + '/packs/000': 'pack',
            }
            for target, source in expected.items():
                data = inputs[source]
                self.assertEqual(files[target], [hashlib.sha256(data).hexdigest(), str(len(data)), '644', source])
            self.assertFalse([target for target in files if target.endswith(('manifest.json', 'coverage.json'))])
            plugin = 'ngpu/addons/TegraGM20BMetal.bundle/TegraGM20BMetal'
            self.assertEqual(files[plugin][2], '755')
            for digest, length, _, source in files.values():
                data = (root / source).read_bytes()
                self.assertEqual(hashlib.sha256(data).hexdigest(), digest)
                self.assertEqual(len(data), int(length))


if __name__ == '__main__':
    unittest.main()
