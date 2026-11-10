// SPDX-License-Identifier: MIT
#pragma once
#include "raw_push.h"

namespace raw {

struct CopySurface {
    uint64_t va = 0;
    uint32_t pitch = 0, x = 0, y = 0, width = 0, height = 0;
    bool blockLinear = false;
    uint8_t blockHeight = 0;
    uint32_t depth = 1, layer = 0;
    uint8_t blockDepth = 0;
};

inline uint32_t copyTexelRemap(uint32_t bpp)
{
    const uint32_t component = bpp < 4 ? bpp : 4;
    const uint32_t count = bpp / component;
    return 0 | 1 << 4 | 2 << 8 | 3 << 12 | (component - 1) << 16 | (count - 1) << 20 | (count - 1) << 24;
}

inline void emitCopy(Push &p, const CopySurface &source, const CopySurface &destination,
                     uint32_t width, uint32_t height, uint32_t remap, bool ordered)
{
    p.m(0x0708, remap, SUBC_COPY);
    p.inc(0x0400, {uint32_t(source.va >> 32), uint32_t(source.va), uint32_t(destination.va >> 32),
                  uint32_t(destination.va), source.pitch, destination.pitch, width, height}, SUBC_COPY);
    uint32_t launch = (ordered ? 2u : 1u) | 1u << 2 | 1u << 9 | 1u << 10;
    const auto surface = [&](const CopySurface &s, uint32_t method, uint32_t pitchBit) {
        if (s.blockLinear)
            p.inc(method, {uint32_t(s.blockHeight) << 4 | uint32_t(s.blockDepth) << 8 | 1u << 12,
                           s.width, s.height, s.depth, s.layer, s.y << 16 | s.x}, SUBC_COPY);
        else
            launch |= pitchBit;
    };
    surface(source, 0x0728, 1u << 7);
    surface(destination, 0x070c, 1u << 8);
    p.m(0x0300, launch, SUBC_COPY);
}

} // namespace raw
