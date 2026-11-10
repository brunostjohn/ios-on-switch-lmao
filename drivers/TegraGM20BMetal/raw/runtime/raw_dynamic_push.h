// SPDX-License-Identifier: MIT
#pragma once
#include "raw_pack.h"
#include <cstdio>
#include <cstring>

namespace raw {

inline uint32_t pushFieldMask(uint32_t word, uint32_t count)
{
    return word && word <= 33 - count ? ((1u << count) - 1) << (word - 1) : 0;
}

inline bool validDynamicPush(uint32_t blend, uint32_t half, uint32_t pushMask)
{
    const uint32_t a = pushFieldMask(blend, 8), b = pushFieldMask(half, 4);
    return (!blend || a) && (!half || b) && !(a & b) && ((a | b) & pushMask) == (a | b);
}

inline bool keyUsesBlendColor(const char *key)
{
    for (const char *at = key; (at = strstr(at, " c")); at += 2) {
        unsigned blend = 0, factors[4] = {};
        const int n =
            sscanf(at, " c%*u=%*u:%*u:%u:%u:%u:%*u:%u:%u", &blend, &factors[0], &factors[1], &factors[2], &factors[3]);
        for (int k = 0; blend && k + 1 < n; k++)
            if (factors[k] >= 11 && factors[k] <= 14)
                return true;
    }
    return false;
}

inline bool usableDynamicPush(const PackPipeline &p, const char *key)
{
    if (!(p.flags & PipelinePushMetadata))
        return !p.blendColorWord && !p.halfBlendColorWord && !keyUsesBlendColor(key);
    return validDynamicPush(p.blendColorWord, p.halfBlendColorWord, p.pushMask);
}

inline void patchBlendPush(const PackPipeline &p, const uint32_t color[4], const uint32_t extended[8],
                           uint32_t push[32])
{
    if (p.blendColorWord)
        memcpy(push + p.blendColorWord - 1, extended, 8 * sizeof(uint32_t));
    if (p.halfBlendColorWord)
        memcpy(push + p.halfBlendColorWord - 1, color, 4 * sizeof(uint32_t));
}

}  // namespace raw
