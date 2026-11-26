"""Unit tests and negative controls for tree_diff.py."""

from __future__ import annotations

import copy
import hashlib
import json
import os
import tempfile
import unittest
from collections.abc import Callable
from pathlib import Path
from typing import Any
from unittest import mock

import tree_diff


def _sample_dump() -> dict[str, Any]:
    sha_a = hashlib.sha256(b'alpha').hexdigest()
    sha_x = hashlib.sha256(b'xval').hexdigest()
    sha_sec = hashlib.sha256(b'acl-bytes').hexdigest()
    return {
        'version': 1,
        'source': 'mount',
        'entries': {
            '/': {
                'type': 'd',
                'uid': 0,
                'gid': 0,
                'mode': '0755',
                'flags': 0,
                'size': 128,
                'sha256': None,
                'xattrs': {},
                'acl': [],
                'symlink_target': None,
                'mtime_ns': 1000,
                'ino': 2,
                'nlink': 2,
            },
            '/a': {
                'type': 'f',
                'uid': 501,
                'gid': 501,
                'mode': '0644',
                'flags': 0x20000,
                'size': 5,
                'sha256': sha_a,
                'xattrs': {
                    'com.example.tag': {'size': 4, 'sha256': sha_x},
                    'com.apple.system.Security': {'size': 9, 'sha256': sha_sec},
                },
                'acl': ['group:ABCDEFAB:everyone:12:deny:delete'],
                'symlink_target': None,
                'mtime_ns': 2000,
                'ino': 10,
                'nlink': 2,
            },
            '/a_link': {
                'type': 'f',
                'uid': 501,
                'gid': 501,
                'mode': '0644',
                'flags': 0x20000,
                'size': 5,
                'sha256': sha_a,
                'xattrs': {
                    'com.example.tag': {'size': 4, 'sha256': sha_x},
                    'com.apple.system.Security': {'size': 9, 'sha256': sha_sec},
                },
                'acl': ['group:ABCDEFAB:everyone:12:deny:delete'],
                'symlink_target': None,
                'mtime_ns': 2000,
                'ino': 10,
                'nlink': 2,
            },
            '/sym': {
                'type': 'l',
                'uid': 0,
                'gid': 0,
                'mode': '0777',
                'flags': 0,
                'size': 1,
                'sha256': None,
                'xattrs': {},
                'acl': [],
                'symlink_target': 'a',
                'mtime_ns': 2000,
                'ino': 11,
                'nlink': 1,
            },
        },
        'hardlink_groups': [['/a', '/a_link']],
    }


class TreeDiffTest(unittest.TestCase):
    def test_dump_captures_files_symlinks_hardlinks_and_identical_diff_passes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'tree'
            (root / 'sub').mkdir(parents=True)
            f = root / 'sub' / 'file.bin'
            f.write_bytes(b'hello oracle\n')
            os.chmod(f, 0o640)
            os.link(f, root / 'sub' / 'file_hl.bin')
            os.symlink('file.bin', root / 'sub' / 'link')

            dump_a = tree_diff.dump_tree(root)
            dump_b = tree_diff.dump_tree(root)
            self.assertEqual(tree_diff.diff_dumps(dump_a, dump_b, tree_diff.DiffOptions(compare_mtime=True)), [])
            self.assertEqual(dump_a['entries']['/sub/file.bin']['mode'], '0640')
            self.assertEqual(
                dump_a['entries']['/sub/file.bin']['sha256'],
                hashlib.sha256(b'hello oracle\n').hexdigest(),
            )
            self.assertEqual(dump_a['entries']['/sub/link']['symlink_target'], 'file.bin')
            self.assertEqual(dump_a['hardlink_groups'], [['/sub/file.bin', '/sub/file_hl.bin']])

    def test_dump_fails_on_read_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'tree'
            root.mkdir()
            (root / 'bad').write_bytes(b'secret')
            with (
                mock.patch.object(tree_diff, '_sha256_file', side_effect=PermissionError('denied')),
                self.assertRaises(PermissionError),
            ):
                tree_diff.dump_tree(root)

    def test_diff_allows_renumbered_inodes_with_same_hardlink_groups(self):
        base = _sample_dump()
        other = copy.deepcopy(base)
        other['entries']['/a']['ino'] = 999
        other['entries']['/a_link']['ino'] = 999
        other['entries']['/sym']['ino'] = 1000
        self.assertEqual(tree_diff.diff_dumps(base, other, tree_diff.DiffOptions(compare_mtime=True)), [])

    def test_plan_vs_mount_acl_consistency(self):
        mount = _sample_dump()
        plan = copy.deepcopy(mount)
        for entry in plan['entries'].values():
            entry['acl'] = None
        self.assertEqual(tree_diff.diff_dumps(plan, mount), [])

        broken = copy.deepcopy(mount)
        broken['entries']['/a']['acl'] = []
        diffs = tree_diff.diff_dumps(plan, broken)
        self.assertTrue(any('/a' in d and 'acl' in d for d in diffs), diffs)

    def test_negative_controls_each_single_mutation_fails(self):
        base = _sample_dump()
        cases: list[tuple[str, dict[str, Any], tree_diff.DiffOptions]] = []

        def add_case(
            label: str,
            mutator: Callable[[dict[str, Any]], object],
            opts: tree_diff.DiffOptions | None = None,
        ) -> None:
            mutated = copy.deepcopy(base)
            mutator(mutated)
            cases.append((label, mutated, opts or tree_diff.DiffOptions()))

        add_case('uid', lambda d: d['entries']['/a'].__setitem__('uid', 0))
        add_case('gid', lambda d: d['entries']['/a'].__setitem__('gid', 80))
        add_case('mode', lambda d: d['entries']['/a'].__setitem__('mode', '0755'))
        add_case('flags', lambda d: d['entries']['/a'].__setitem__('flags', 0x20))
        add_case('xattr_val', lambda d: d['entries']['/a']['xattrs']['com.example.tag'].__setitem__('sha256', '0' * 64))
        add_case('xattr_missing', lambda d: d['entries']['/a']['xattrs'].pop('com.example.tag'))
        add_case('acl', lambda d: d['entries']['/a'].__setitem__('acl', []))
        add_case('byte', lambda d: d['entries']['/a'].__setitem__('sha256', hashlib.sha256(b'alphb').hexdigest()))
        add_case('size', lambda d: d['entries']['/a'].__setitem__('size', 6))
        add_case('symlink', lambda d: d['entries']['/sym'].__setitem__('symlink_target', 'other'))
        add_case('hardlink_split', lambda d: d['entries']['/a_link'].__setitem__('ino', 777))
        add_case('missing_path', lambda d: d['entries'].pop('/sym'))
        add_case(
            'mtime',
            lambda d: d['entries']['/a'].__setitem__('mtime_ns', 2001),
            tree_diff.DiffOptions(compare_mtime=True),
        )

        for label, mutated, opts in cases:
            with self.subTest(label=label):
                diffs = tree_diff.diff_dumps(base, mutated, opts)
                self.assertTrue(diffs, f'negative control {label!r} unexpectedly passed')

        # mtime difference is ignored unless --compare-mtime is passed.
        mtime_only = copy.deepcopy(base)
        mtime_only['entries']['/a']['mtime_ns'] = 9999
        self.assertEqual(tree_diff.diff_dumps(base, mtime_only), [])

    def test_cli_diff_exit_codes_and_ignore_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            p1 = Path(tmp) / 'exp.json'
            p2 = Path(tmp) / 'act.json'
            exp = _sample_dump()
            act = copy.deepcopy(exp)
            act['entries']['/.fseventsd'] = {
                'type': 'd',
                'uid': 0,
                'gid': 0,
                'mode': '0700',
                'flags': 0,
                'size': 64,
                'sha256': None,
                'xattrs': {},
                'acl': [],
                'symlink_target': None,
                'mtime_ns': 1000,
                'ino': 50,
                'nlink': 2,
            }
            p1.write_text(json.dumps(exp), encoding='utf-8')
            p2.write_text(json.dumps(act), encoding='utf-8')
            self.assertEqual(tree_diff.main(['diff', str(p1), str(p2)]), 1)
            self.assertEqual(tree_diff.main(['diff', str(p1), str(p2), '--ignore-path', '/.fseventsd']), 0)


if __name__ == '__main__':
    unittest.main()
