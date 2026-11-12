#!/usr/bin/env python3
"""SwitchGM20BMacroSlots.h's NVK slots are nvk_mme.h's: NVK loads its macros in its enum's order, slot by slot.

The kernel's push profile reads NVK's clear and anti-alias calls and both drivers' draws by these numbers, and the raw
path's draw macros take NVK's draw macros' slots: a Mesa update that moves NVK's enum fails here first.
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).parents[3]
SLOTS = ROOT / 'kits/SwitchGM20BEmit/SwitchGM20BMacroSlots.h'
NVK = ROOT / 'third_party/mesa/src/nouveau/vulkan/nvk_mme.h'


def nvk_slots() -> dict[str, int]:
    """Give each NVK_MME_* macro its slot, its place in enum nvk_mme."""
    body = re.search(r'enum nvk_mme \{(.*?)\};', NVK.read_text(), re.DOTALL).group(1)
    names = re.findall(r'^\s*(NVK_MME_\w+),', body, re.MULTILINE)
    return {name: slot for slot, name in enumerate(names)}


def header_slots() -> dict[str, tuple[int, str]]:
    """Give each slot the header numbers explicitly its value and the NVK macro its comment names."""
    found = {}
    for name, value, nvk in re.findall(r'(SWITCH_GM20B_MACRO_\w+) = (\d+),\s*// (NVK_MME_\w+)', SLOTS.read_text()):
        found[name] = (int(value), nvk)
    return found


class MacroSlots(unittest.TestCase):
    def test_each_nvk_slot_is_nvk_mmes(self) -> None:
        nvk = nvk_slots()
        named = header_slots()
        self.assertEqual(
            set(named),
            {
                'SWITCH_GM20B_MACRO_NVK_CLEAR',
                'SWITCH_GM20B_MACRO_NVK_SET_ANTI_ALIAS',
                'SWITCH_GM20B_MACRO_DRAWS_FIRST',
                'SWITCH_GM20B_MACRO_DRAWS_LAST',
            },
        )
        for name, (value, macro) in named.items():
            self.assertEqual(nvk[macro], value, f'{name} names {macro}, which NVK loads at slot {nvk[macro]}')

    def test_nvks_draws_are_the_range_the_profile_counts(self) -> None:
        nvk = nvk_slots()
        draws = [slot for name, slot in nvk.items() if name.startswith('NVK_MME_DRAW') and 'MESH' not in name]
        named = header_slots()
        self.assertEqual(min(draws), named['SWITCH_GM20B_MACRO_DRAWS_FIRST'][0])
        self.assertEqual(max(draws), named['SWITCH_GM20B_MACRO_DRAWS_LAST'][0])
        self.assertEqual(len(draws), max(draws) - min(draws) + 1)


if __name__ == '__main__':
    unittest.main()
