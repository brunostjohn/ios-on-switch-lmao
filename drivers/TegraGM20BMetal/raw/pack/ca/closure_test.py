"""closure.py: the names one field away from the seeds, along the fields the drawn names vary."""

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import caset  # noqa: E402
import caspec  # noqa: E402
import closure  # noqa: E402


class Closure(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.t = caspec.Tables(os.path.join(HERE, 'qc-24B119.json'))

    def tier(self, seeds: list[str], drawn: list[str]) -> list[str]:
        return closure.tier(self.t, seeds, drawn, False, lambda n: caset.makes(self.t, n))

    def test_a_name_is_its_fields(self):
        for name in ('PBGRABsovXmw_TmuaA3S1Xhfu_Ialp_Isqr', 'PBGRAXmw_TimgA3Xhfs_IxrgN3Oc3mtc4nlnlnl', 'PBGRAXm_A2Xhf'):
            self.assertEqual(closure.name_of(closure.fields(self.t, name)), name)

    def test_a_texture_function_drawn_beside_another_is_tried_on_every_seed(self):
        drawn = ['PBGRAXm_TimgA2Xhf_Isrc', 'PBGRAXm_TcimA2Xhf_Isrc']
        self.assertIn('PBGRABsovXm_TcimA2Xhf_Iues', self.tier(['PBGRABsovXm_TimgA2Xhf_Iues'], drawn))

    def test_an_image_function_drawn_beside_another_is_tried_in_each_image(self):
        drawn = ['PBGRAXm_TimgA2Xhf_Isrc', 'PBGRAXm_TimgA2Xhf_Isar']
        made = self.tier(['PBGRABsovXm_TmuaA2Xhf_Isrc_Isqr'], drawn)
        self.assertIn('PBGRABsovXm_TmuaA2Xhf_Isar_Isqr', made)

    def test_flags_the_device_or_target_set_are_not_varied(self):
        drawn = ['PBGRAXm_TimgA2Xhf_Isrc', 'PBGRAXm_TimgA2Xhfc_Isrc', 'PBGRAXm_TimgA2Xhfn_Isrc']
        made = self.tier(['PBGRAXm_TcimA2Xhf_Ialp'], drawn)
        self.assertIn('PBGRAXm_TcimA2Xhfn_Ialp', made)
        self.assertNotIn('PBGRAXm_TcimA2Xhfc_Ialp', made)

    def test_a_seed_the_device_names_otherwise_seeds_nothing(self):
        drawn = ['PBGRAXm_A3Xhf', 'PBGRAXm_A3Xhfn']
        self.assertEqual(self.tier(['PBGRAXm_A3Xhfc'], drawn), [])


if __name__ == '__main__':
    unittest.main()
