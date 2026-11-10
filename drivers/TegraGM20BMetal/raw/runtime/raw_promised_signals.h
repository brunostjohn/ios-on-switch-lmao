// SPDX-License-Identifier: MIT
// The signals right after a raw range are promised at its submission and published at its completion (RawMetalEvents.mm
// publishRawEventSignals), so a command buffer that ends in signals leaves its queue's head with its last raw range instead
// of holding it until its GPU work completes. The completion thread retires raw ranges in submission order, so when this
// range completes every earlier raw range of the command buffer has, and the promise succeeds only if none of them failed.
#pragma once
#include <cstddef>

namespace raw {

enum class RangeKind {
    native,
    compatibility,
    signal,
    wait,
};

// [at + 1, returned): the signal ranges right after native range `at` (of `count`, `kindAt(i)` each) that its completion
// publishes. None (at + 1) where an earlier range went to m2v, whose completion is not ordered before this one's; where the
// command buffer's raw ranges did not all run on this range's channel (`oneChannel`: a channel made again between them); or
// where it failed already.
template <typename KindAt>
size_t promisedSignalsEnd(KindAt &&kindAt, size_t count, size_t at, bool oneChannel, bool failed)
{
    const size_t none = at + 1;
    if (failed || !oneChannel)
        return none;
    for (size_t i = 0; i < at; ++i)
        if (kindAt(i) == RangeKind::compatibility)
            return none;
    size_t end = none;
    while (end < count && kindAt(end) == RangeKind::signal)
        ++end;
    return end;
}

}  // namespace raw
