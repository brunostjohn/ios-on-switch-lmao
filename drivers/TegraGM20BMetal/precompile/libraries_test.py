import contextlib
import io
import json
import tempfile
import unittest
from pathlib import Path

import libraries

STAGED = '/private/var/staged_system_apps/Example.app/Extensions/Scene.appex/default.metallib'
INSTALLED = (
    '/private/var/containers/Bundle/Application/01234567-89AB-CDEF-0123-456789ABCDEF/'
    'Example.app/Extensions/Scene.appex/default.metallib'
)


class ResolutionTest(unittest.TestCase):
    def test_installed_system_app_uses_the_selected_os_staged_library(self):
        self.assertEqual(libraries.library_path(INSTALLED, {STAGED}), STAGED)
        self.assertEqual(libraries.library_path(INSTALLED.removeprefix('/private'), {STAGED}), STAGED)

    def test_missing_or_distinct_application_is_not_guessed(self):
        self.assertEqual(libraries.library_path(INSTALLED, set()), INSTALLED)
        other = INSTALLED.replace('Example.app', 'Other.app')
        self.assertEqual(libraries.library_path(other, {STAGED}), other)
        digest = 'data:' + '1' * 64
        self.assertEqual(libraries.library_path(digest, {STAGED}), digest)

    def test_exact_installed_library_takes_precedence(self):
        self.assertEqual(libraries.library_path(INSTALLED, {STAGED, INSTALLED}), INSTALLED)

    def test_cryptex_and_escaped_paths_resolve_without_changing_constants(self):
        path = '/System/Library/Frameworks/Example Kit.framework/default.metallib'
        line = 'R ok=1 | v entry lib=/System/Cryptexes/OS' + path.replace(' ', '%20') + ' #7:33:02000000 | '
        self.assertEqual(libraries.relocate(line, {path}), line.replace('/System/Cryptexes/OS', ''))

    def test_dispatch_and_linked_stages_resolve_together(self):
        table = {'kernel': [STAGED], 'helper': [STAGED]}
        lines = [
            'D ok=1 tg=32,1,1 edge=0,0,0 threads=0 | form=d opt=0 | k kernel | l helper | ',
            f'R ok=1 | v kernel lib={INSTALLED} | f - | fl helper lib={INSTALLED} as=selected | ',
        ]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'names.json').write_text(json.dumps(table))
            (root / 'input.log').write_text('\n'.join(lines) + '\n')
            with contextlib.redirect_stdout(io.StringIO()):
                libraries.resolve(str(root / 'names.json'), str(root / 'input.log'), str(root / 'output.log'))
            result = (root / 'output.log').read_text().splitlines()
        self.assertEqual(
            result[0],
            lines[0].replace('k kernel', 'k kernel lib=' + STAGED).replace('l helper', 'l helper lib=' + STAGED),
        )
        self.assertEqual(result[1], lines[1].replace(INSTALLED, STAGED))


if __name__ == '__main__':
    unittest.main()
