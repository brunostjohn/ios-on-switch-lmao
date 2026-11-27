# SPDX-License-Identifier: MIT
"""Adjusts Windows MSVC bindgen output to match Unix enum signedness and packed bool bitfields."""

import re
import sys
from pathlib import Path

TYPE_INT = re.compile(r'^pub type (\w+) = ::std::os::raw::c_int;$', re.MULTILINE)
CONST_VAL = re.compile(r'^pub const \w+: (\w+) = ([^;]+);$', re.MULTILINE)
BOOL_BITFIELD = re.compile(
    r'(pub fn (?:params_from_src1|sat)\(&self\) -> )u32'
    r'(\s*\{\s*unsafe\s*\{\s*::std::mem::transmute\('
    r'self\._bitfield_1\.get_const::<\d+usize, 1u8>\(\) as )u32'
    r'(\)\s*\}\s*\})',
)


def fix_bindings(text: str) -> str:
    values: dict[str, list[str]] = {}
    for match in CONST_VAL.finditer(text):
        values.setdefault(match.group(1), []).append(match.group(2).strip())

    unsigned_enums = {name for name, vals in values.items() if vals and all(not val.startswith('-') for val in vals)}

    def replace_enum(match: re.Match[str]) -> str:
        name = match.group(1)
        if name in unsigned_enums:
            return f'pub type {name} = ::std::os::raw::c_uint;'
        return match.group(0)

    text = TYPE_INT.sub(replace_enum, text)
    return BOOL_BITFIELD.sub(r'\1bool\2u8\3', text)


def main() -> None:
    path = Path(sys.argv[1])
    path.write_text(fix_bindings(path.read_text()))


if __name__ == '__main__':
    main()
