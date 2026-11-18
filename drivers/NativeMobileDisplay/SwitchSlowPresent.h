// SPDX-License-Identifier: BSD-2-Clause-Patent
// Debug kernels: the cause NativeLegacyDisplay charges a present two or more refresh periods after the one before it
// to, in its pacing line ("long"), its SwitchFrame lines and the touch interaction line. Times are ticks of one
// monotonic clock.
#pragma once
#include <stdint.h>

namespace switch_slow_present {

enum Cause : unsigned {
    kIdle,   // the compositor turned vsync off since the previous present: it had nothing to draw (no stall)
    kCa,     // its swap came at or after the boundary a one-frame interval would have used: the compositor was late
    kGpu,    // the swap came in time, but kick to flip alone outlasted the time it had left: its GPU work ended late
    kQueue,  // in time, but waiting behind the previous copy or for a surface made it miss
    kLatch,  // the flip was asked for in time but taken a boundary later
    kRate,   // its swap came under half a period after the vsync it answered: the frames before had nothing new (r17)
    // Its swap came late with no frame end in the two periods before it: FRAME_END was masked (the display idle), so
    // nothing tells a late compositor from one that had nothing to draw until then.
    kMasked,
    kCauses
};

inline constexpr const char *kNames[kCauses] = {"idle", "ca", "gpu", "queue", "latch", "rate", "masked"};

// A present's swap: its submission, its kick and its flip (0: not known), and the vsync it came after, if one came in
// the two periods before its submission (0: none).
struct Swap {
    uint64_t submit, kick, ready, vsync;
};

inline Cause cause(const Swap &s, uint64_t lastPresent, uint64_t period, bool idleSincePresent)
{
    const uint64_t due = lastPresent + period;  // the boundary a one-frame interval would have used
    if (!s.submit || s.submit >= due) {
        if (idleSincePresent)
            return kIdle;
        if (s.vsync && s.submit - s.vsync < period / 2)
            return kRate;
        if (s.submit && !s.vsync)
            return kMasked;
        return kCa;
    }
    if (s.ready > due)
        return s.ready - s.kick > due - s.submit ? kGpu : kQueue;
    return kLatch;
}

}  // namespace switch_slow_present
