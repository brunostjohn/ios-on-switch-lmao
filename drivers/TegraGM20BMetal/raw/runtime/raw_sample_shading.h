// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

namespace raw {

inline bool nativeSamples(uint32_t samples)
{
    return samples == 1 || samples == 2 || samples == 4 || samples == 8;
}

inline bool sampleShadingMasks(uint32_t samples, uint32_t passes, uint32_t words[4])
{
    if (!nativeSamples(samples) || (passes != 1 && passes != samples))
        return false;
    for (uint32_t pair = 0; pair < 4; pair++)
        words[pair] = passes == samples ? (1u << (2 * pair)) | (1u << (2 * pair + 17)) : ~0u;
    return true;
}

inline uint32_t sampleShadingControl(uint32_t passes)
{
    return passes | (passes > 1 ? 0x10 : 0);
}

// SET_PIXEL_SHADER_INTERLOCK_CONTROL `interlock` as drawn under SET_HYBRID_ANTI_ALIAS_CONTROL `hybrid`. The GM20B kills a
// draw that detects conflicts per pixel while it runs more than one pass a pixel: GR DATA_ERROR 0x134
// (INVALID_A_A_COMBINATION03 by the gv100 class error table) at its first DRAW_VERTEX_ARRAY. Upstream NVK takes sample
// detection there (Mesa MR 38206), as the raw stage compile does since 54b64321f; a record made before then keeps pixel
// detection, which this corrects as it is drawn.
inline uint32_t legalInterlockControl(uint32_t interlock, uint32_t hybrid)
{
    constexpr uint32_t kMode = 3, kSample = 1, kPixel = 2, kPasses = 0xf;
    if ((hybrid & kPasses) > 1 && (interlock & kMode) == kPixel)
        return (interlock & ~kMode) | kSample;
    return interlock;
}

}  // namespace raw
