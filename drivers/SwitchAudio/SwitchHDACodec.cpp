// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SwitchHDA.h"

bool SwitchHDA::discover(uint32_t formats)
{
    const uint32_t present = core(0x0e) & 0x7fff;
    if (!present)
        return false;
    uint32_t value = 0;
    afg = converter = pin = 0;
    uint32_t converterCaps = 0;
    for (codec = 0; codec < 15; ++codec) {
        if (!(present & (1u << codec)))
            continue;
        if (!verb(0, 0xf00, 0, &value))
            return false;
        if (value != 0x10de0029)
            continue;
        if (!verb(0, 0xf00, 4, &value))
            return false;
        const uint32_t groups = value & 0xff, first = (value >> 16) & 0xff;
        afg = 0;
        for (uint32_t node = first; node < first + groups && node < 256; ++node) {
            if (!verb(node, 0xf00, 5, &value))
                return false;
            if ((value & 0xff) == 1) {
                afg = node;
                break;
            }
        }
        if (!afg || !verb(afg, 0x705, 0) || !verb(afg, 0xf00, 4, &value))
            return false;
        const uint32_t widgets = value & 0xff, begin = (value >> 16) & 0xff;
        for (uint32_t node = begin; node < begin + widgets && node < 256; ++node) {
            if (!verb(node, 0xf00, 9, &value))
                return false;
            if (((value >> 20) & 15) != 4 || !(value & 0x200))
                continue;
            if (!verb(node, 0xf09, 0, &value))
                return false;
            if ((value & 0xc0000000u) != 0xc0000000u)
                continue;
            pin = node;
            break;
        }
        if (pin)
            break;
    }
    if (!pin || !verb(pin, 0xf02, 0, &value))
        return false;
    converter = value & 0x7f;
    if (!converter || !verb(converter, 0xf00, 9, &converterCaps) || ((converterCaps >> 20) & 15) ||
        (converterCaps & 0x201) != 0x201)
        return false;
    const uint32_t formatNode = converterCaps & 16 ? converter : afg;
    if (!verb(formatNode, 0xf00, 0x0b, &value) || !(value & 1) || !verb(formatNode, 0xf00, 0x0a, &value) ||
        !(value & (1u << 6)))
        return false;
    control->sample_bits = switch_hda_sample_bits(formats, value);
    if (!control->sample_bits)
        return false;
    const uint32_t stripe = (core(0) >> 1) & 3;
    if (!verb(converter, 0x200, switch_hda_stream_format(control->sample_bits)) || !verb(converter, 0x706, 0x10) ||
        !verb(converter, 0x70d, 1) || !verb(converter, 0x70e, 0) || !verb(converter, 0x72d, 1) ||
        !verb(converter, 0x724, stripe ? 1 : 0) || !verb(pin, 0x707, 0x40))
        return false;
    return attached;
}
