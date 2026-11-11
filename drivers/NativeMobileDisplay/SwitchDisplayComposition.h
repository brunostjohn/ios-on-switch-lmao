// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "NativePlanes.h"
#include "../SwitchPower/SwitchDisplayBandwidth.h"

namespace switch_display {
struct Overlay {
    bool on = false;
    int slot = -1;
    native_planes::Image image = {};
    unsigned depth = 2;
};

struct Composition {
    Overlay window[2] = {};
    uint64_t seq = 0, pendingSeq = 0;

    bool on() const { return window[0].on || window[1].on; }

    bool uses(int slot) const
    {
        return slot >= 0 && ((window[0].on && window[0].slot == slot) ||
                            (window[1].on && window[1].slot == slot));
    }
};

template<class State = Composition> class Compositions {
public:
    static constexpr unsigned kSlots = 4;
    static constexpr int kNone = -2, kOff = -1;
    enum class Latch { Waiting, Taken, Replaced, Inconsistent };
    State *values = nullptr;
    int active = kOff, armed = kNone, superseded = kNone;

    bool requested() const { return armed != kNone; }
    bool mayWrite() const { return superseded == kNone; }
    bool busy(int id) const { return id >= 0 && (id == active || id == armed || id == superseded); }

    int write(const State &next)
    {
        if (!mayWrite())
            return kNone;
        unsigned id = 0;
        while (id < kSlots && busy(int(id)))
            ++id;
        if (id == kSlots)
            return kNone;
        values[id] = next;
        superseded = armed;
        armed = int(id);
        return armed;
    }

    Latch observe(uint32_t requests, int scanned)
    {
        if (!requested())
            return Latch::Waiting;
        if (!requests) {
            if (scanned != armed)
                return Latch::Inconsistent;
            active = armed;
            armed = superseded = kNone;
            return Latch::Taken;
        }
        if (superseded != kNone && scanned == superseded) {
            active = superseded;
            superseded = kNone;
            return Latch::Replaced;
        }
        return Latch::Waiting;
    }

    bool latched(bool requested, int scanned)
    {
        const auto result = observe(requested ? 1 : 0, scanned);
        return result == Latch::Taken || result == Latch::Replaced;
    }

    void reset()
    {
        active = kOff;
        armed = superseded = kNone;
    }

    void settle()
    {
        if (requested())
            active = armed;
        armed = superseded = kNone;
    }

    bool uses(int slot) const
    {
        for (unsigned id = 0; id < kSlots; ++id)
            if (busy(int(id)) && values[id].uses(slot))
                return true;
        return false;
    }
};

struct WindowState {
    bool live = false;
    int slot = -1;
    int matrix = -1, fullRange = -1;
};

struct Assignment {
    unsigned window[2] = {0, 1};
    unsigned convert = 0;
};

inline Assignment assign(const Overlay wanted[2], const WindowState current[2])
{
    Assignment best;
    unsigned bestCost = UINT32_MAX;
    for (unsigned permutation = 0; permutation != 2; ++permutation) {
        Assignment candidate = {{permutation, permutation ^ 1u}, 0};
        unsigned cost = 0;
        for (unsigned p = 0; p != 2; ++p) {
            if (!wanted[p].on)
                continue;
            const auto &window = current[candidate.window[p]];
            const auto &image = wanted[p].image;
            const bool cscMatches = window.matrix == int(image.matrix) && window.fullRange == int(image.fullRange);
            if (image.convert || (image.yuv && window.live && !cscMatches)) {
                candidate.convert |= 1u << p;
                cost += 16;
            }
            if (window.slot != wanted[p].slot)
                cost += 2;
            if (image.yuv && !cscMatches)
                ++cost;
        }
        if (cost < bestCost) {
            best = candidate;
            bestCost = cost;
        }
    }
    return best;
}

inline uint64_t transitionPeak(const Demand &ui, const Demand *first, unsigned firstCount,
                               const Demand *second, unsigned secondCount)
{
    if (!first || !second || !firstCount || !secondCount || firstCount > 4 || secondCount > 4)
        return UINT64_MAX;
    uint64_t maximum = 0;
    for (unsigned a = 0; a < firstCount; ++a)
        for (unsigned b = 0; b < secondCount; ++b) {
            const Demand windows[] = {ui, first[a], second[b]};
            const uint64_t bytes = peak(windows, 3);
            if (bytes > maximum)
                maximum = bytes;
        }
    return maximum;
}
} // namespace switch_display
