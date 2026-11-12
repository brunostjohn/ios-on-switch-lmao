// SPDX-License-Identifier: MIT
// See raw_multisample.h and Device::createStoredMultisample, createOwnedZeta (raw_gpu.h).
#include <algorithm>
#include "raw_gpu.h"
#include "raw_multisample.h"

namespace raw {

bool Device::createStoredMultisample(uint32_t width, uint32_t height, uint32_t format, uint32_t samples, Texture &t,
                                     const ArenaAt *at)
{
    TextureAsk ask;
    ask.kind = TextureAsk::Multisample;
    ask.width = width;
    ask.height = height;
    ask.samples = samples;
    ask.format = format;
    return createLaidOut(ask, t, at);
}

bool Device::createOwnedZeta(uint32_t width, uint32_t height, uint32_t format, uint32_t samples, bool sampled,
                             Texture &t, const ArenaAt *at)
{
    if (samples < 2 && !at)
        return false;
    TextureAsk ask;
    ask.kind = TextureAsk::Zeta;
    ask.width = width;
    ask.height = height;
    ask.samples = samples;
    ask.format = format;
    ask.sampled = sampled;
    return createLaidOut(ask, t, at);
}

}  // namespace raw
