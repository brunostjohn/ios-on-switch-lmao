// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
#include <string.h>

namespace switch_display {

// IOMFBSwap layout read by IOMobileFramebufferLegacy::swap_submit on both 21H461 and 24B119:
// surfaceID[4] at +0x9c, enabled layer mask at +0x14c, swap type at +0x17c (9 uses an internal surface).
// Core Animation unconditionally sets enabled bit (layer == 0 ? 2 : 0) with surfaceID 0 on every detached-plane
// frame, causing Apple's swap_submit to call IOSurfaceRoot::lookupSurface(root, 0) on every frame.
// Eliding bits for layers already known off preserves swapEffectiveSurfaces identically while avoiding lookupSurface(0).
struct SwapZeroLayerState {
    static constexpr unsigned kSwapSurfaceIdsOffset = 0x9c;
    static constexpr unsigned kSwapEnabledOffset = 0x14c;
    static constexpr unsigned kSwapTypeOffset = 0x17c;
    static constexpr uint32_t kSwapTypeInternal = 9;
    static constexpr unsigned kLayers = 4;
    static constexpr uint32_t kLayerMask = 0xfu;

    struct Call {
        uint32_t origEnabled = 0;
        uint32_t elided = 0;
        uint32_t activating = 0;
        uint32_t disabling = 0;
        uint32_t preSeq[kLayers] = {};
        bool special = false;
    };

    uint32_t activeMask = kLayerMask;
    uint32_t turningOn[kLayers] = {};
    uint32_t turnOnSeq[kLayers] = {};

    Call begin(unsigned char *swap)
    {
        Call c;
        if (!swap)
            return c;
        uint32_t swapType = 0;
        memcpy(&c.origEnabled, swap + kSwapEnabledOffset, sizeof(c.origEnabled));
        memcpy(&swapType, swap + kSwapTypeOffset, sizeof(swapType));
        if (swapType == kSwapTypeInternal) {
            c.special = true;
            activeMask = kLayerMask;
            for (unsigned i = 0; i < kLayers; ++i)
                ++turnOnSeq[i];
            return c;
        }
        uint32_t surfaceIds[kLayers] = {};
        memcpy(surfaceIds, swap + kSwapSurfaceIdsOffset, sizeof(surfaceIds));
        for (unsigned i = 0; i < kLayers; ++i) {
            const uint32_t bit = 1u << i;
            if ((c.origEnabled & bit) == 0)
                continue;
            if (surfaceIds[i] != 0) {
                c.activating |= bit;
                activeMask |= bit;
                ++turningOn[i];
                ++turnOnSeq[i];
            } else if ((activeMask & bit) == 0 && turningOn[i] == 0) {
                c.elided |= bit;
            } else {
                c.disabling |= bit;
                c.preSeq[i] = turnOnSeq[i];
            }
        }
        if (c.origEnabled != 0 && (c.origEnabled & ~c.elided) == 0) {
            for (unsigned i = 0; i < kLayers; ++i) {
                if (c.elided & (1u << i)) {
                    c.disabling |= (1u << i);
                    c.preSeq[i] = turnOnSeq[i];
                }
            }
            c.elided = 0;
        }
        if (c.elided != 0) {
            const uint32_t filtered = c.origEnabled & ~c.elided;
            memcpy(swap + kSwapEnabledOffset, &filtered, sizeof(filtered));
        }
        return c;
    }

    void finish(unsigned char *swap, const Call &c, bool succeeded)
    {
        if (swap && c.elided != 0)
            memcpy(swap + kSwapEnabledOffset, &c.origEnabled, sizeof(c.origEnabled));
        if (c.special) {
            activeMask = kLayerMask;
            return;
        }
        for (unsigned i = 0; i < kLayers; ++i) {
            const uint32_t bit = 1u << i;
            if (c.activating & bit)
                --turningOn[i];
            if ((c.disabling & bit) && succeeded && turningOn[i] == 0 && turnOnSeq[i] == c.preSeq[i])
                activeMask &= ~bit;
        }
    }

    void reset()
    {
        activeMask = kLayerMask;
        for (unsigned i = 0; i < kLayers; ++i)
            ++turnOnSeq[i];
    }
};

}  // namespace switch_display
