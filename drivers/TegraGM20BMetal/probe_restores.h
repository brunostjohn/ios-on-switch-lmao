// SPDX-License-Identifier: MIT
// The registers HW Test's probe words (TegraGM20BMetalProbe.mm switchMetalProbeBefore:after:) may write that a state's
// words do not set at its bind: a probe draw that wrote one sets it back to the GM20B golden context's value
// (nvidia/gm20b/gr/sw_method_init.bin), which every later draw assumes, so no probe leaves hardware state behind.
#pragma once
#include <cstdint>
#include "../../kits/SwitchGM20B/SwitchGM20B3D.h"

namespace gm20b {

struct ProbeRestore {
    uint32_t method, value;
};

constexpr ProbeRestore kProbeRestores[] = {
    {NVB197_SET_SAMPLE_MASK, 0x1},  // raster out on, colour target off
    {NVB197_SET_COLOR_TARGET_SAMPLE_MASK, 0xffff},
    {NVB197_SET_CONSTANT_COLOR_RENDERING, 0},
    {NVB197_SET_CONSTANT_COLOR_RENDERING_RED, 0},
    {NVB197_SET_CONSTANT_COLOR_RENDERING_GREEN, 0},
    {NVB197_SET_CONSTANT_COLOR_RENDERING_BLUE, 0},
    {NVB197_SET_CONSTANT_COLOR_RENDERING_ALPHA, 0},
    {NVB197_SET_PIPELINE_SHADER(5), 0x51},  // the pixel stage on (a stage-off state's restore words say so too)
};

// kProbeRestores' entries `method` writes, bit i entry i
inline uint32_t probeRestoreBits(uint32_t method)
{
    uint32_t bits = 0;
    for (uint32_t i = 0; i < sizeof kProbeRestores / sizeof *kProbeRestores; i++)
        bits |= uint32_t(kProbeRestores[i].method == method) << i;
    return bits;
}

inline bool probeWritesRasterMasks(uint32_t method)
{
    return method >= NVB197_SET_SAMPLE_MASK_X0_Y0 && method <= NVB197_SET_SAMPLE_MASK_X1_Y1;
}

}  // namespace gm20b
