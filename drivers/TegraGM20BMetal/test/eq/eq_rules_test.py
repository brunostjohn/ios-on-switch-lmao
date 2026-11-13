"""eq_rules.py's classes on draws made by hand: a pair of one NVK draw and one raw draw of a single colour target."""

import unittest

import eq_fields
import eq_rules

TARGET = {'rt0': 'rt0=N1 64x64 fmt=d5 mem=1000 pitch=0'}
WRITES = {'1a00': '1111'}
FETCH_BARRIERS = {'PSB', 'TEXINV'}


def draw(registers: dict, fetches: bool, barriers: set) -> eq_fields.Draw:
    """Give a draw of the target whose fragment stage reaches the target's texture header where it fetches it."""
    content = {'handle{tic[N1 00000000 00000000]/tsc[00000000 00000000]}'} if fetches else set()
    return eq_fields.Draw(dict(TARGET), WRITES | registers, {'stage5': eq_fields.Stage(content=content)}, barriers)


def compare(nvk: eq_fields.Draw, raw: eq_fields.Draw) -> tuple[set, list[str]]:
    tags, refused = eq_rules.compare(eq_fields.Frame([nvk], [], []), eq_fields.Frame([raw], [], []))
    return tags[0] if tags else set(), refused


# NVK's fetching form: its program blends, the ROP's blend of colour 0 off at ZERO coefficients
NVK_FETCH = {'1e08': '4000', '1e0c': '4000'}
# raw's fetch-free form of an additive blend: SET_BLEND(0) on, ONE and ONE
RAW_ROP_BLEND = {'1360': '1', '1e08': '4001', '1e0c': '4001'}


class FetchFree(unittest.TestCase):
    def test_a_colour_raw_blends_on_the_rop_where_only_nvk_fetches_it_is_raws_form(self):
        tags, refused = compare(draw(NVK_FETCH, True, FETCH_BARRIERS), draw(RAW_ROP_BLEND, False, set()))
        self.assertEqual(refused, [])
        self.assertEqual(tags, {'fetchfree'})

    def test_raw_blending_on_the_rop_while_it_still_fetches_is_refused(self):
        _, refused = compare(draw(NVK_FETCH, True, FETCH_BARRIERS), draw(RAW_ROP_BLEND, True, set()))
        self.assertIn('draw 0: register 1360 nvk=- raw=1', refused)

    def test_a_rop_blend_where_nvk_fetches_nothing_is_refused(self):
        _, refused = compare(draw(NVK_FETCH, False, set()), draw(RAW_ROP_BLEND, False, set()))
        self.assertIn('draw 0: register 1360 nvk=- raw=1', refused)


class NoRead(unittest.TestCase):
    def test_a_draw_that_reads_no_colour_takes_none_of_nvks_fetch_barriers(self):
        tags, refused = compare(draw({}, True, FETCH_BARRIERS), draw({}, False, set()))
        self.assertEqual(refused, [])
        self.assertEqual(tags, {'noread'})

    def test_a_draw_that_still_fetches_without_the_barrier_is_refused(self):
        _, refused = compare(draw({}, True, FETCH_BARRIERS), draw({}, True, set()))
        self.assertEqual(refused, ["draw 0: barriers ['PSB', 'TEXINV'] only in nvk"])

    def test_other_barriers_only_nvk_has_are_refused(self):
        _, refused = compare(draw({}, True, {'WFI'}), draw({}, False, set()))
        self.assertEqual(refused, ["draw 0: barriers ['WFI'] only in nvk"])


if __name__ == '__main__':
    unittest.main()
