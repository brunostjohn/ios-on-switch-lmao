// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

enum {
    SWITCH_HDA_S16 = 1,
    SWITCH_HDA_S24 = 4,
    SWITCH_HDA_PERIOD_FRAMES = 16384
};

static inline uint32_t switch_hda_sink_formats(const uint8_t *edid, uint32_t valid_blocks)
{
    uint32_t formats = 0;
    if (!edid || !(valid_blocks & 1u))
        return 0;
    for (uint32_t block = 1; (valid_blocks >>= 1) != 0; ++block) {
        if (!(valid_blocks & 1u))
            continue;
        const uint8_t *cta = edid + block * 128;
        if (cta[0] != 2)
            continue;
        if (cta[3] & 0x40)
            formats |= SWITCH_HDA_S16;
        const uint32_t end = cta[2];
        if (end < 4 || end > 127)
            continue;
        for (uint32_t at = 4; at < end;) {
            const uint32_t tag = cta[at] >> 5, length = cta[at] & 31;
            ++at;
            if (length > end - at)
                break;
            if (tag == 1) {
                for (uint32_t sad = at; sad + 3 <= at + length; sad += 3) {
                    if ((cta[sad] >> 3) == 1 && (cta[sad] & 7) >= 1 && (cta[sad + 1] & 4))
                        formats |= cta[sad + 2] & (SWITCH_HDA_S16 | SWITCH_HDA_S24);
                }
            }
            at += length;
        }
    }
    return formats;
}

static inline uint32_t switch_hda_sample_bits(uint32_t sink_formats, uint32_t pcm)
{
    if ((sink_formats & SWITCH_HDA_S24) && (pcm & (1u << 19)))
        return 24;
    if ((sink_formats & SWITCH_HDA_S16) && (pcm & (1u << 17)))
        return 16;
    return 0;
}

static inline uint16_t switch_hda_stream_format(uint32_t bits)
{
    return (uint16_t)((bits == 24 ? 3u : 1u) << 4 | 1u);
}

static inline uint32_t switch_hda_verb(uint32_t codec, uint32_t node, uint32_t verb, uint32_t value)
{
    return codec << 28 | node << 20 | verb << 8 | value;
}

static inline uint32_t switch_hda_eld(const uint8_t *edid, uint32_t valid_blocks, uint8_t eld[128])
{
    for (uint32_t byte = 0; byte < 128; ++byte)
        eld[byte] = 0;
    if (!switch_hda_sink_formats(edid, valid_blocks))
        return 0;
    uint32_t name = 0, count = 0;
    for (uint32_t at = 54; at + 18 <= 126; at += 18) {
        if (edid[at] || edid[at + 1] || edid[at + 2] || edid[at + 3] != 0xfc || edid[at + 4])
            continue;
        while (name < 13 && edid[at + 5 + name] != 10 && edid[at + 5 + name]) {
            eld[20 + name] = edid[at + 5 + name];
            ++name;
        }
        break;
    }
    uint32_t revision = 0;
    for (uint32_t block = 1; (valid_blocks >>= 1) != 0; ++block) {
        if (!(valid_blocks & 1u))
            continue;
        const uint8_t *cta = edid + block * 128;
        if (cta[0] != 2)
            continue;
        if (cta[1] > revision)
            revision = cta[1];
        const uint32_t end = cta[2];
        if (end < 4 || end > 127)
            continue;
        for (uint32_t at = 4; at < end;) {
            const uint32_t tag = cta[at] >> 5, length = cta[at] & 31;
            ++at;
            if (length > end - at)
                break;
            if (tag == 1) {
                for (uint32_t sad = at; sad + 3 <= at + length && count < 15; sad += 3) {
                    for (uint32_t byte = 0; byte < 3; ++byte)
                        eld[20 + name + count * 3 + byte] = cta[sad + byte];
                    ++count;
                }
            } else if (tag == 4 && length) {
                eld[7] |= cta[at];
            }
            at += length;
        }
    }
    if (!count) {
        eld[20 + name] = 0x09;
        eld[21 + name] = 7;
        eld[22 + name] = 1;
        count = 1;
    }
    eld[0] = 2u << 3;
    eld[2] = (uint8_t)((16 + name + count * 3 + 3) / 4);
    eld[4] = (uint8_t)(((revision & 7) << 5) | name);
    eld[5] = (uint8_t)((count << 4) | (1u << 2));
    eld[7] |= 1;
    for (uint32_t byte = 0; byte < 4; ++byte)
        eld[16 + byte] = edid[8 + byte];
    return 4u + eld[2] * 4u;
}
