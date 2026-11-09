"""raw/runtime/raw_zeta.h: each depth/stencil format's SET_ZT_FORMAT and texture header word 0 are what NVK writes.

NVK takes both from NIL's row of the image's format (nil_formats.csv through nil_format_table_gen.py), and samples a
depth aspect through a view that keeps the image's format (nvk_image_view.c). The words are rebuilt here from that
row and the class headers by the generator's rules.
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
ZETA = ROOT / 'drivers/TegraGM20BMetal/raw/runtime/raw_zeta.h'
NIL = ROOT / 'third_party/mesa/src/nouveau/nil/nil_formats.csv'
CLASSES = ROOT / 'third_party/mesa/src/nouveau/headers/nvidia/classes'

# metal2vulkan's Vulkan format of each Metal depth/stencil format, as a pipe format
PIPE = {250: 'Z16_UNORM', 252: 'Z32_FLOAT', 253: 'S8_UINT', 260: 'Z32_FLOAT_S8X24_UINT'}


def defines(path: Path) -> dict[str, int]:
    out = {}
    for line in path.read_text().splitlines():
        m = re.match(r'#define\s+(\w+)\s+(0x[0-9a-fA-F]+)\s*$', line)
        if m:
            out[m.group(1)] = int(m.group(2), 16)
    return out


def nil_rows() -> dict[str, list[str]]:
    rows = {}
    for line in NIL.read_text().splitlines():
        if not line.strip() or line.startswith('#'):
            continue
        fields = [f.strip() for f in line.split(',')]
        rows[fields[0]] = fields
    return rows


def tic0(row: list[str], tex: dict[str, int]) -> int:
    _, _, tcs, types, _, srcs = row[:6]
    word = tex['NVB197_TEXHEAD_BL_COMPONENTS_SIZES_' + tcs]
    names = {'S': 'SNORM', 'N': 'UNORM', 'I': 'SINT', 'U': 'UINT', 'F': 'FLOAT'}
    for c in range(4):
        kind = types[c] if c < len(types) else types[0]
        word |= tex['NV9097_TEXHEADV2_0_R_DATA_TYPE_NUM_' + names[kind]] << (7 + 3 * c)
    for c, s in enumerate(srcs):
        if s == '1':
            first = next(types['RGBA'.find(x)] for x in srcs if x in 'RGBA')
            value = tex['NV9097_TEXHEADV2_0_Z_SOURCE_IN_ONE_' + ('FLOAT' if first in 'SNF' else 'INT')]
        else:
            value = tex['NV9097_TEXHEADV2_0_Z_SOURCE_IN_' + {'0': 'ZERO'}.get(s, s)]
        word |= value << (19 + 3 * c)
    return word


class RawZetaTicTest(unittest.TestCase):
    def test_zeta_words_are_nils(self):
        tex = defines(CLASSES / 'clb197tex.h') | defines(CLASSES / 'cl9097tex.h')
        threed = defines(CLASSES / 'clb197.h')
        rows = nil_rows()
        table = re.findall(
            r'\{(\d+), (0x[0-9a-f]+), \d+, 0x[0-9a-f]+, (?:true|false), (0x[0-9a-f]+)\}', ZETA.read_text()
        )
        self.assertTrue(table)
        for metal, zt, word in table:
            row = rows[PIPE[int(metal)]]
            with self.subTest(metal=metal, pipe=row[0]):
                self.assertEqual(int(zt, 16), threed['NVB197_SET_ZT_FORMAT_V_' + row[1]])
                self.assertEqual(int(word, 16), tic0(row, tex))


if __name__ == '__main__':
    unittest.main()
