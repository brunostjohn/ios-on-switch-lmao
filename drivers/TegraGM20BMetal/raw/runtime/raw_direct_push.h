// SPDX-License-Identifier: MIT
#pragma once
#include "bridge/NativeMetalDirect.h"
#include "raw_dynamic_push.h"

namespace raw {

inline bool validDynamicPush(const NmbDirectState &state)
{
    for (const NmbDirectDraw &d : state.draws)
        if (!validDynamicPush(d.blendColorWord, d.halfBlendColorWord, d.pushMask) ||
            d.blendColorWord != state.draws[0].blendColorWord ||
            d.halfBlendColorWord != state.draws[0].halfBlendColorWord ||
            d.depthTestWord != state.draws[0].depthTestWord || d.stencilTestWord != state.draws[0].stencilTestWord)
            return false;
    for (const NmbDirectDraw &d : state.draws) {
        const uint32_t depth = pushFieldMask(d.depthTestWord, 4), stencil = pushFieldMask(d.stencilTestWord, 4);
        const uint32_t blend = pushFieldMask(d.blendColorWord, 8) | pushFieldMask(d.halfBlendColorWord, 4);
        if (bool(d.depthTestWord) != bool(state.storage.depthFormat) ||
            bool(d.stencilTestWord) != bool(state.storage.stencilFormat) || (d.depthTestWord && !depth) ||
            (d.stencilTestWord && !stencil) || (depth & stencil) || ((depth | stencil) & blend) ||
            ((depth | stencil) & ~d.pushMask))
            return false;
    }
    return true;
}

// The push words a draw's addresses fill (raw_heap_tables.h): each stage's root-table and ExecutionStatus address, two words
// each
inline uint32_t addressPushWords(const NmbDirectDraw &d)
{
    uint32_t mask = 0;
    for (uint32_t k = 0; k < 2; k++)
        mask |= pushFieldMask(d.rootTableWord[k], 2) | pushFieldMask(d.statusWord[k], 2);
    return mask;
}

// The bind-offset draw changes every buffer residual; only words shared by all probe draws can be folded. An address hole is
// 0 in every probe draw and the draw's address in a real one.
inline uint32_t constantPushWords(const NmbDirectState &state)
{
    uint32_t mask = state.draws[0].pushMask;
    for (const NmbDirectDraw &d : state.draws) {
        mask &= ~(pushFieldMask(d.blendColorWord, 8) | pushFieldMask(d.halfBlendColorWord, 4) |
                  pushFieldMask(d.depthTestWord, 4) | pushFieldMask(d.stencilTestWord, 4) | addressPushWords(d));
        for (uint32_t i = 0; i < 32; i++)
            if (!(d.pushMask >> i & 1) || d.push[i] != state.draws[0].push[i])
                mask &= ~(1u << i);
    }
    return mask;
}

}  // namespace raw
