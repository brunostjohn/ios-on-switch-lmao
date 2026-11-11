import hashlib
import tempfile
import unittest
from pathlib import Path
from tools.gpu.rawpack import library_view, own_build


class NativeLibraries(unittest.TestCase):
    def test_crawled_and_harvested_libraries_share_content_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sources = []
            for name, content in [('embedded', b'embedded AIR'), ('device', b'CI AIR'), ('duplicate', b'CI AIR')]:
                source = root / name
                source.write_bytes(content)
                sources.append(source)
            view = library_view(sources, root / 'view')
            self.assertEqual(len(list(view.iterdir())), 2)
            for source in sources:
                digest = hashlib.sha256(source.read_bytes()).hexdigest()
                self.assertEqual((view / (digest + '.metallib')).read_bytes(), source.read_bytes())

    def test_missing_declared_input_fails_instead_of_silently_losing_coverage(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaises(FileNotFoundError):
                library_view([root / 'missing'], root / 'view')


STATE = (
    'samples=1 topo=0 ds=260/260 c0=80:15:0:1:0:0:1:0:0 | v a lib=/x/default.metallib  | f b lib=/x/default.metallib | '
)


class BuildTags(unittest.TestCase):
    def test_a_pack_takes_its_own_builds_lines_and_untagged_ones_and_counts_the_rest(self):
        lines = [
            f'R ok=1 label=x os=24B119 {STATE}',
            f'R ok=1 label=x os=21H461 {STATE}',
            f'R ok=1 label=x {STATE}',
            'D ok=1 tg=64,1,1 edge=64,1,1 threads=64 os=21H461 | form=0 | k sum',
        ]
        taken, report = own_build(lines, '24B119')
        self.assertEqual(taken, [f'R ok=1 label=x {STATE}', f'R ok=1 label=x {STATE}'])
        self.assertEqual(
            report,
            ['corpus for 24B119: 2 lines taken, 1 of them untagged (no os=); 2 of another build left out (21H461 2)'],
        )

    def test_a_functions_constants_are_not_a_build_tag(self):
        line = 'R ok=1 label=x samples=1 topo=0 ds=0/0 | v a os=1  | f b | '
        self.assertEqual(own_build([line], '24B119')[0], [line])


if __name__ == '__main__':
    unittest.main()
