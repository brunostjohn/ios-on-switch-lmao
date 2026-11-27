#!/usr/bin/env python3
"""Unit tests for pack_gpu_asset.py."""

import hashlib
import io
import tarfile
import tempfile
import unittest
from pathlib import Path

from install.image_gpu import pack_gpu_asset


class PackGpuAssetTest(unittest.TestCase):
    def _make_base(self, root: Path) -> Path:
        bundle = root / 'ngpu/addons/TegraGM20BMetal.bundle'
        bundle.mkdir(parents=True)
        plugin = b'macho-plugin-bytes'
        plist = b'<plist/>'
        (bundle / 'TegraGM20BMetal').write_bytes(plugin)
        (bundle / 'Info.plist').write_bytes(plist)
        (root / 'TegraGM20BMetal.trustcache').write_bytes(b'ignored-in-gpu-tar')
        lines = [
            '# gpu stage 2 tree',
            f'file\tngpu/addons/TegraGM20BMetal.bundle/Info.plist\t{hashlib.sha256(plist).hexdigest()}\t{len(plist)}\t644\tngpu/addons/TegraGM20BMetal.bundle/Info.plist',
            f'file\tngpu/addons/TegraGM20BMetal.bundle/TegraGM20BMetal\t{hashlib.sha256(plugin).hexdigest()}\t{len(plugin)}\t755\tngpu/addons/TegraGM20BMetal.bundle/TegraGM20BMetal',
            'pref\tmobile/Library/Preferences/com.apple.backboardd.plist\t501\tCAEnableDeepFramebuffer\tfalse',
        ]
        (root / 'gpu.stage').write_text('\n'.join(lines) + '\n', encoding='utf-8')
        return root

    def test_deterministic_tar_injects_kext_and_excludes_trustcache(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            base = self._make_base(Path(td))
            digest = 'ab' * 32
            first = pack_gpu_asset.build_tar(base, digest + '\n')
            second = pack_gpu_asset.build_tar(base, digest)
            self.assertEqual(first, second)

            with tarfile.open(fileobj=io.BytesIO(first), mode='r:') as tar:
                members = tar.getmembers()
                names = [m.name for m in members]
                self.assertEqual(
                    names,
                    [
                        'gpu.stage',
                        'ngpu/addons/TegraGM20BMetal.bundle/Info.plist',
                        'ngpu/addons/TegraGM20BMetal.bundle/TegraGM20BMetal',
                    ],
                )
                modes = {m.name: m.mode for m in members}
                self.assertEqual(modes['gpu.stage'], 0o644)
                self.assertEqual(modes['ngpu/addons/TegraGM20BMetal.bundle/Info.plist'], 0o644)
                self.assertEqual(modes['ngpu/addons/TegraGM20BMetal.bundle/TegraGM20BMetal'], 0o755)
                for m in members:
                    self.assertEqual((m.uid, m.gid, m.mtime, m.uname, m.gname), (0, 0, 0, '', ''))
                manifest_text = tar.extractfile('gpu.stage').read().decode('utf-8')
                manifest_lines = manifest_text.splitlines()
                self.assertEqual(manifest_lines[0], '# gpu stage 2 tree')
                self.assertEqual(manifest_lines[1], f'# kext {digest}')
                self.assertEqual(len(manifest_lines), 5)

    def test_refuses_corrupted_file_and_bad_digest(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            base = self._make_base(Path(td))
            with self.assertRaises(ValueError):
                pack_gpu_asset.build_tar(base, '   ')
            (base / 'ngpu/addons/TegraGM20BMetal.bundle/Info.plist').write_bytes(b'<tamper>')
            with self.assertRaises(ValueError):
                pack_gpu_asset.build_tar(base, 'cd' * 32)

    def test_accepts_absolute_sandbox_symlinks_and_rejects_relative_symlinks(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            real = self._make_base(root / 'real')
            sandbox = root / 'sandbox'
            for src in real.rglob('*'):
                rel = src.relative_to(real)
                dst = sandbox / rel
                if src.is_dir():
                    dst.mkdir(parents=True, exist_ok=True)
                else:
                    dst.parent.mkdir(parents=True, exist_ok=True)
                    dst.symlink_to(src.resolve())
            digest = 'ef' * 32
            self.assertEqual(pack_gpu_asset.build_tar(real, digest), pack_gpu_asset.build_tar(sandbox, digest))

            plist = sandbox / 'ngpu/addons/TegraGM20BMetal.bundle/Info.plist'
            plist.unlink()
            plist.symlink_to('TegraGM20BMetal')
            with self.assertRaises(ValueError):
                pack_gpu_asset.build_tar(sandbox, digest)


if __name__ == '__main__':
    unittest.main()
