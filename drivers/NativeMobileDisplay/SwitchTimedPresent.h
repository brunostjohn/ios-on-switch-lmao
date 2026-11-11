// SPDX-License-Identifier: BSD-2-Clause-Patent
// When a swap's requested presentation lets it go. Core Animation stamps every swap with the vsync it rendered for
// (swapRequestedPresentation, mach continuous time); presentAtTime: and presentAfterMinimumDuration: move that later.
// DC A latches a flip only at a frame end, so a flip armed now shows from the next frame end at the earliest: a swap
// waits while that frame end is more than half a period before its target.
#pragma once
#include <stdint.h>

namespace switch_timed_present {

struct Clock {
    uint64_t nowAbsolute, nowContinuous;
    uint64_t lastFrameEnd;  // mach absolute time of the last DC A frame end the filter saw
    uint64_t period;        // the frame period, mach absolute units
    uint64_t horizon;       // a target further out than this is not one the render server holds for: shown as now
};

enum class Decision {
    Due,
    Early,       // the next frame end is too early for it
    NeedsPhase,  // the frame ends were masked: the next one says which frame end is the target's
};

inline Decision decide(uint64_t requestedContinuous, const Clock &c)
{
    if (!requestedContinuous || !c.period || c.nowContinuous < c.nowAbsolute)
        return Decision::Due;
    const uint64_t asleep = c.nowContinuous - c.nowAbsolute;
    if (requestedContinuous <= asleep)
        return Decision::Due;
    const uint64_t requested = requestedContinuous - asleep;
    if (requested <= c.nowAbsolute + c.period / 2 || requested - c.nowAbsolute > c.horizon)
        return Decision::Due;
    if (!c.lastFrameEnd || c.lastFrameEnd > c.nowAbsolute || c.nowAbsolute - c.lastFrameEnd >= 2 * c.period)
        return Decision::NeedsPhase;
    const uint64_t nextFrameEnd = c.lastFrameEnd + ((c.nowAbsolute - c.lastFrameEnd) / c.period + 1) * c.period;
    return requested > nextFrameEnd + c.period / 2 ? Decision::Early : Decision::Due;
}

}  // namespace switch_timed_present
