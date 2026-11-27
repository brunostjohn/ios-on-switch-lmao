import hashlib
import io
import json
import struct
import tarfile
import tempfile
import unittest
from pathlib import Path

from install.seed.assets import pack_seed_asset


def _fake_metallib(name: bytes = b'test_fn') -> bytes:
    header = bytearray(0x58)
    header[0:4] = b'MTLB'
    body = struct.pack('<II', 1, 16) + b'NAME' + struct.pack('<H', len(name) + 1) + name + b'\0ENDT'
    struct.pack_into('<QQ', header, 0x18, 0x58, len(body))
    struct.pack_into('<Q', header, 0x10, 0x58 + len(body))
    return bytes(header) + body


def _make_base_inputs(root: Path) -> tuple[pack_seed_asset.AssetInputs, Path, bytes]:
    manifest = root / 'MANIFEST.json'
    manifest.write_text(json.dumps({'stack': '0123456789abcdef'}) + '\n', encoding='utf-8')
    own = root / 'own.metallib'
    own.write_bytes(_fake_metallib())
    context_log = root / 'context.log'
    context_log.write_text('0x0000 0x00000001\n', encoding='utf-8')
    draw_context = root / 'draw_context.txt'
    draw_context.write_text('0x0004 0x00000002\n', encoding='utf-8')
    corpus = root / 'corpus.log'
    corpus.write_text('R ok=1 os=21H461\n', encoding='utf-8')
    archives = root / '21H461.tsv'
    archives.write_text('', encoding='utf-8')
    report = root / 'corpus-report.txt'
    report.write_text('corpus for 21H461: 1 lines taken\n', encoding='utf-8')
    buckets: list[Path] = []
    for idx in range(17):
        p = root / f'bucket{idx}.log'
        p.write_text(f'line {idx}\n' * (idx % 3), encoding='utf-8')
        buckets.append(p)
    app_lib = root / 'app.metallib'
    app_lib_bytes = _fake_metallib(b'hwtest_fn')
    app_lib.write_bytes(app_lib_bytes)
    inputs = pack_seed_asset.AssetInputs(
        build='21H461',
        kind='21H461',
        manifest=manifest,
        qc_sha256='a' * 64,
        own_metallib=own,
        context_log=context_log,
        draw_context=draw_context,
        corpus=corpus,
        archives=archives,
        embedded=[Path('drivers/TegraGM20BMetal/plugin/TwoPlaneStandIn.h')],
        corpus_report=report,
        split_buckets=buckets,
        app_libraries=(
            ('Applications/SwitchHWTest27e.app/default.metallib', app_lib),
            ('Applications/SwitchHWTest.app/default.metallib', app_lib),
        ),
    )
    return inputs, app_lib, app_lib_bytes


class PackSeedAssetTest(unittest.TestCase):
    def test_extract_real_two_plane_stand_in(self) -> None:
        header = Path('drivers/TegraGM20BMetal/plugin/TwoPlaneStandIn.h')
        digest, data = pack_seed_asset.extract_embedded_header(header)
        self.assertEqual(len(digest), 64)
        self.assertEqual(data[:4], b'MTLB')
        self.assertEqual(hashlib.sha256(data).hexdigest(), digest)

    def test_build_deterministic_tar(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            inputs, _, app_lib_bytes = _make_base_inputs(Path(tmp))
            app_lib_sha = hashlib.sha256(app_lib_bytes).hexdigest()
            tar_a = pack_seed_asset.build_tar(inputs)
            tar_b = pack_seed_asset.build_tar(inputs)
            self.assertEqual(tar_a, tar_b)

            with tarfile.open(fileobj=io.BytesIO(tar_a), mode='r') as tar:
                members = tar.getmembers()
                names = [m.name for m in members]
                self.assertEqual(names, sorted(names))
                self.assertEqual(names.count(f'raw/apps/{app_lib_sha}.metallib'), 1)
                for member in members:
                    self.assertEqual(
                        (member.uid, member.gid, member.mtime, member.uname, member.gname), (0, 0, 0, '', '')
                    )
                    self.assertEqual(member.mode, 0o644)

                parsed = json.loads(tar.extractfile('manifest.json').read().decode('utf-8'))
                self.assertEqual(parsed['format'], 1)
                self.assertEqual(parsed['build'], '21H461')
                self.assertEqual(parsed['stack_key'], '0123456789abcdef')
                self.assertEqual(parsed['quartzcore_metallib_sha256'], 'a' * 64)
                self.assertEqual(parsed['bucket_counts']['split'], 17)
                self.assertEqual(len(parsed['embedded']), 1)
                self.assertEqual(
                    parsed['app_libraries'],
                    [
                        {
                            'guest_path': 'Applications/SwitchHWTest.app/default.metallib',
                            'path': f'raw/apps/{app_lib_sha}.metallib',
                            'sha256': app_lib_sha,
                            'size': len(app_lib_bytes),
                        },
                        {
                            'guest_path': 'Applications/SwitchHWTest27e.app/default.metallib',
                            'path': f'raw/apps/{app_lib_sha}.metallib',
                            'sha256': app_lib_sha,
                            'size': len(app_lib_bytes),
                        },
                    ],
                )
                self.assertEqual(len(parsed['files']), len(names) - 1)

    def test_rejects_invalid_app_libraries(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            inputs, app_lib, _ = _make_base_inputs(Path(tmp))
            bad_outside = inputs._replace(app_libraries=(('System/Library/foo.metallib', app_lib),))
            with self.assertRaises(ValueError):
                pack_seed_asset.build_tar(bad_outside)

            bad_case_dup = inputs._replace(
                app_libraries=(
                    ('Applications/SwitchHWTest.app/default.metallib', app_lib),
                    ('Applications/switchhwtest.app/default.metallib', app_lib),
                )
            )
            with self.assertRaises(ValueError):
                pack_seed_asset.build_tar(bad_case_dup)


if __name__ == '__main__':
    unittest.main()
