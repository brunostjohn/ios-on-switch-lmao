// SPDX-License-Identifier: MIT
#pragma once
#include "raw_pack.h"
#include "raw_zeta.h"
#include <cstring>

namespace raw {

inline uint32_t shaderStencilWord(const StencilFace &face)
{
    return (face.compare & 7u) | (face.fail & 7u) << 3 | (face.pass & 7u) << 6 | (face.readMask & 255u) << 9 |
           (face.writeMask & 255u) << 17 | (face.depthFail & 7u) << 25;
}

inline void patchShaderTestPush(const PackShaderTests &tests, const DepthStencilDesc &state,
                                const uint32_t references[2], const float depthRange[2], bool clamp, uint32_t push[32])
{
    if (tests.depthWord) {
        auto *words = push + tests.depthWord - 1;
        words[0] = (state.depthCompare & 7u) | uint32_t(state.depthWrite) << 3 | uint32_t(clamp) << 4;
        memcpy(words + 1, depthRange, 2 * sizeof(float));
        words[3] = 0;
    }
    if (tests.stencilWord) {
        auto *words = push + tests.stencilWord - 1;
        words[0] = shaderStencilWord(state.front);
        words[1] = shaderStencilWord(state.back);
        words[2] = (references[0] & 255u) | (references[1] & 255u) << 8;
        words[3] = 0;
    }
}

}  // namespace raw
