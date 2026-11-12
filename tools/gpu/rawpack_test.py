import hashlib
import os
import tempfile
import unittest
from pathlib import Path

from tools.gpu.rawpack import OS_BUILD, framework_line, library_view, own_build, probe_lines


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
    def test_a_pack_takes_the_lines_of_its_build_and_counts_the_rest(self):
        lines = [
            f'R ok=1 label=x os=24B119 {STATE}',
            f'R ok=1 label=x os=21H461 {STATE}',
            f'R ok=1 label=x os=21H461,24B119 {STATE}',
            'D ok=1 tg=64,1,1 edge=64,1,1 threads=64 os=21H461 | form=0 | k sum',
        ]
        taken, report = own_build(lines, '24B119')
        self.assertEqual(taken, [f'R ok=1 label=x {STATE}', f'R ok=1 label=x {STATE}'])
        self.assertEqual(report, ['corpus for 24B119: 2 lines taken; 2 of other builds left out (21H461 2)'])

    def test_a_line_that_names_no_build_fails_the_step(self):
        with self.assertRaises(SystemExit):
            own_build([f'R ok=1 label=x os=24B119 {STATE}', f'R ok=1 label=x {STATE}'], '24B119')

    def test_a_functions_constants_are_not_a_build_tag(self):
        line = 'R ok=1 label=x samples=1 topo=0 ds=0/0 | v a os=1  | f b | '
        with self.assertRaises(SystemExit):
            own_build([line], '24B119')


class CorpusTags(unittest.TestCase):
    """Every line of the checked-in corpus a pack reads names the builds it is a state of."""

    def test_every_render_and_dispatch_line_names_known_builds(self):
        corpus = Path(os.environ['RAWPACK_CORPUS']).read_text().splitlines()
        builds = set(os.environ['RAWPACK_BUILDS'].split())
        lines = probe_lines('\n'.join(corpus))
        self.assertGreater(len(lines), 1000)
        for line in lines:
            tag = OS_BUILD.search(line.split(' | ', 1)[0])
            self.assertIsNotNone(tag, line[:200])
            self.assertLessEqual(set(tag[1].split(',')), builds, line[:200])


def lines_of(vertex: str, fragment: str, linked: str = '') -> str:
    return f'R ok=1 label=- samples=1 topo=0 ds=0/0 | v a lib={vertex}  | f b lib={fragment} {linked}| '


class Lookups(unittest.TestCase):
    FRAMEWORK = '/System/Library/PrivateFrameworks/RenderBox.framework/default.metallib'
    BUNDLE = '/System/Library/VideoProcessors/NRFV1.bundle/default.metallib'
    RESOURCES = '/System/Library/PrivateFrameworks/AppleCVAPhoto.framework/Resources/default.metallib'

    def test_a_framework_library_and_a_bundles_are_looked_up(self):
        for library in (self.FRAMEWORK, self.BUNDLE, self.RESOURCES):
            self.assertTrue(framework_line(lines_of(library, library)), library)

    def test_quartzcore_an_apps_and_mixed_libraries_are_not(self):
        quartzcore = '/System/Library/Frameworks/QuartzCore.framework/default.metallib'
        app = '/Applications/SwitchHWTest.app/default.metallib'
        realitykit = '/System/Library/PrivateFrameworks/CoreRE.framework/mxi.metallib'
        for vertex, fragment in [
            (quartzcore, quartzcore),
            (app, app),
            (realitykit, realitykit),
            (self.BUNDLE, self.RESOURCES),
        ]:
            self.assertFalse(framework_line(lines_of(vertex, fragment)), (vertex, fragment))

    def test_a_bundle_state_links_nothing(self):
        linked = f'| fl c lib={self.BUNDLE} '
        self.assertFalse(framework_line(lines_of(self.BUNDLE, self.BUNDLE, linked)))


if __name__ == '__main__':
    unittest.main()
