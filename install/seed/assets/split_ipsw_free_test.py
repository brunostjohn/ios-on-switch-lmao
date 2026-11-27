import tempfile
import unittest
from pathlib import Path

from install.seed.assets import split_ipsw_free


class SplitIpswFreeTest(unittest.TestCase):
    def test_framework_and_app_line(self) -> None:
        fw = (
            'R ok=1 samples=1 c0=80:15:0:1:0:0:1:0:0 '
            '| v v_fn lib=/System/Library/PrivateFrameworks/RenderBox.framework/default.metallib '
            '| f f_fn lib=/System/Library/PrivateFrameworks/RenderBox.framework/default.metallib '
        )
        self.assertTrue(split_ipsw_free.framework_line(fw))
        self.assertFalse(split_ipsw_free.app_line(fw))

        app = (
            'R ok=1 samples=1 c0=80:15:0:1:0:0:1:0:0 '
            '| v v_fn lib=/Applications/SwitchHWTest.app/default.metallib '
            '| f f_fn lib=/Applications/SwitchHWTest.app/default.metallib '
        )
        self.assertFalse(split_ipsw_free.framework_line(app))
        self.assertTrue(split_ipsw_free.app_line(app))

    def test_stand_in_and_alpha_one(self) -> None:
        line = (
            'R ok=1 samples=1 c0=500:15:1:8:9:0:10:8:0 '
            '| v v_fn lib=/System/Library/PrivateFrameworks/RenderBox.framework/default.metallib '
            '| f f_fn lib=/System/Library/PrivateFrameworks/RenderBox.framework/default.metallib '
        )
        mapped = split_ipsw_free.stand_in(line)
        self.assertIn(' c0=125:15:1:1:0:0:1:1:0 ', mapped)

    def test_own_build_keeps_other_ca_for_names(self) -> None:
        lines = [
            'R ok=1 os=21H461 label=com.apple.coreanimation.A samples=1 | v va | f fa ',
            'R ok=1 os=24B119 label=com.apple.coreanimation.B samples=1 | v vb | f fb ',
            'D ok=1 os=24B119 | k kern lib=/System/Library/Frameworks/CoreImage.framework/default.metallib ',
        ]
        out, report = split_ipsw_free.own_build(lines, '21H461')
        self.assertEqual(
            out,
            [
                'R ok=1 label=com.apple.coreanimation.A samples=1 | v va | f fa ',
                'R ok=1 os=24B119 label=com.apple.coreanimation.B samples=1 | v vb | f fb ',
            ],
        )
        self.assertEqual(len(report), 1)
        self.assertIn('1 lines taken; 2 of other builds left out (24B119 2)', report[0])
        self.assertIn('1 Core Animation lines kept for their names', report[0])

    def test_split_with_unknown_digest_writes_all_buckets(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            corpus = root / 'corpus.log'
            corpus.write_text(
                'R ok=1 os=21H461 label=com.apple.coreanimation.A samples=1 '
                '| v v_main lib=/System/Library/Frameworks/QuartzCore.framework/default.metallib '
                '| f downsample_4_frag lib=/System/Library/Frameworks/QuartzCore.framework/default.metallib \n',
                encoding='utf-8',
            )
            prefs = root / 'stage_format.py'
            prefs.write_text('PREFS = []\n', encoding='utf-8')
            report = root / 'corpus-report.txt'
            outs = [root / f'bucket{i}.log' for i in range(17)]
            split_ipsw_free.split_ipsw_free(
                split_ipsw_free.SplitInputs(
                    corpus_inputs=split_ipsw_free.CorpusInputs(
                        corpus=corpus,
                        build='21H461',
                        archives=None,
                        app_states=None,
                    ),
                    qc_sha256='0' * 64,
                    ca_names=None,
                    ca_preferences=prefs,
                    report=report,
                    outs=outs,
                )
            )
            self.assertTrue(report.is_file())
            self.assertEqual(len(outs), 17)
            for out in outs:
                self.assertIn('f downsample_4_frag', out.read_text(encoding='utf-8'))


if __name__ == '__main__':
    unittest.main()
