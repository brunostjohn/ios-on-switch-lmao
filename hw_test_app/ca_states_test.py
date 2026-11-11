"""ca_states.py over the corpus and the selected IPSW's QuartzCore: each build's sweep asks for its own states only."""

import os
import unittest
from pathlib import Path

from hw_test_app import ca_states

LINE = (
    'R ok=1 label=com.apple.coreanimation.PBGRAXm_A2Xhf samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0 '
    '| v {vertex} lib=/System/Library/Frameworks/QuartzCore.framework/default.metallib {constants} '
    '| f {fragment}  | '
)


class States(unittest.TestCase):
    def setUp(self) -> None:
        self.build = os.environ['CA_BUILD']
        self.corpus = Path(os.environ['CA_CORPUS']).read_text()
        self.library = Path(os.environ['CA_LIBRARY']).read_bytes()

    def test_only_ios_seventeen_keeps_its_stroke_states(self) -> None:
        kept = ca_states.states(self.corpus, self.library)
        strokes = [line for line in kept if '| v path_stroke_' in line]
        if self.build == '21H461':
            self.assertGreater(len(strokes), 0)
        else:
            self.assertEqual(strokes, [], 'iOS 27 has no path_stroke_* functions')
        self.assertGreater(len(kept), 100)

    def test_a_constant_the_function_does_not_declare_is_another_builds_state(self) -> None:
        declared = {'std_vert1_lph': {'fc_generic'}, 'luma_boost_lph': set()}
        own = LINE.format(vertex='std_vert1_lph', constants='fc_generic:53:00', fragment='luma_boost_lph')
        other = LINE.format(vertex='std_vert1_lph', constants='fc_perspective:53:00', fragment='luma_boost_lph')
        missing = LINE.format(vertex='path_stroke_line_vert_lph', constants='', fragment='luma_boost_lph')
        self.assertTrue(ca_states.made_here(own, declared))
        self.assertFalse(ca_states.made_here(other, declared))
        self.assertFalse(ca_states.made_here(missing, declared))


if __name__ == '__main__':
    unittest.main()
