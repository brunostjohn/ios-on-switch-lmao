// SPDX-License-Identifier: MIT
// What a state's draws (or a kernel's dispatches) cost the GPU, learned from GPU time stamps of its own large draws, which the
// band decision (draw_bands.h, dispatch_bands.h) reads in place of the instruction-count estimate once it has one.
// The static estimate prices a draw as pixels x program instructions, so a draw bound by memory or the ROP (a 4x resolve:
// 642 instructions, priced 3 ms, measured 11.5-12 ms) or a looped program is priced far short and kept whole.
//
// A unit is a pixel of the draw's box times its sample passes and instances (a thread of a dispatch). Only a draw alone in
// its pass (a dispatch alone in its encoder) of at least kMeasureUnits units is a candidate: its stamps go where the pass
// already drains the tiled cache and waits (draw_bands.cpp), so a measured draw costs what an unmeasured one does. A
// state's first kWarmSamples candidates, then one in kRemeasureEvery, are measured, counted where they are encoded whatever
// later teaches them; the next command buffer's begin teaches every committed measured draw the GPU has run
// (Device::learnCompleted). Picks, stamp slots and learning are the encoding thread's alone (the device encodes one command
// buffer at a time), so the pick count is a load and a store; the rate stays atomic for the band decision's relaxed reads.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>

namespace gm20b {

constexpr uint64_t kMeasureUnits = 64 * 1024;
constexpr uint32_t kWarmSamples = 3;
constexpr uint32_t kRemeasureEvery = 32;
constexpr uint32_t kMeasuredPerCommandBuffer = 16;
constexpr uint32_t kRateWeight = 4;  // a new sample moves a warm rate by a quarter of the difference

struct BandRate {
    std::atomic<uint32_t> psPerUnit{0};  // a unit's share of a draw's time, its pixels' (0: not measured yet)
    // what each band of a banded draw repeats: its vertices and the drain at its end (0: not measured yet)
    std::atomic<uint32_t> nsPerBand{0};
    std::atomic<uint32_t> samples{0};
    std::atomic<uint32_t> picks{0};

    BandRate() = default;

    BandRate(const BandRate &o)
    {
        *this = o;
    }

    BandRate &operator=(const BandRate &o)
    {
        psPerUnit.store(o.psPerUnit.load(std::memory_order_relaxed), std::memory_order_relaxed);
        nsPerBand.store(o.nsPerBand.load(std::memory_order_relaxed), std::memory_order_relaxed);
        samples.store(o.samples.load(std::memory_order_relaxed), std::memory_order_relaxed);
        picks.store(o.picks.load(std::memory_order_relaxed), std::memory_order_relaxed);
        return *this;
    }
};

inline uint64_t unitsNs(uint64_t units, uint64_t ps)
{
    return units > UINT64_MAX / ps ? UINT64_MAX / 1000 : units * ps / 1000;
}

// The work's GPU time: `units` at the learned rate, else the static estimate
inline uint64_t estimatedNs(const BandRate &r, uint64_t units, uint64_t staticNs)
{
    const uint64_t ps = r.psPerUnit.load(std::memory_order_relaxed);
    return ps ? unitsNs(units, ps) : staticNs;
}

// Whether any of its draws was measured, whole or banded
inline bool measuredOnce(const BandRate &r)
{
    return r.psPerUnit.load(std::memory_order_relaxed) || r.nsPerBand.load(std::memory_order_relaxed);
}

inline bool picked(uint32_t pick)
{
    return pick < kWarmSamples || (pick - kWarmSamples) % kRemeasureEvery == kRemeasureEvery - 1;
}

// Whether a candidate of `units` is one to measure; counts it where it is large enough to be one
inline bool measure(BandRate &r, uint64_t units)
{
    if (units < kMeasureUnits)
        return false;
    const uint32_t pick = r.picks.load(std::memory_order_relaxed);
    r.picks.store(pick + 1, std::memory_order_relaxed);
    return picked(pick);
}

// Whether the state's next candidate would be measured, without counting it (a dispatch's start stamp goes out before its
// encoder knows the dispatch is alone)
inline bool wouldMeasure(const BandRate &r, uint64_t units)
{
    return units >= kMeasureUnits && picked(r.picks.load(std::memory_order_relaxed));
}

// A measured draw: `ns` of GPU time over `units`, drawn in `bands` (1: whole); `staticPixelNs` its pixels' instruction
// count price. Each band repeats the draw's vertices and drains at its end, so a banded draw takes its pixels' time plus
// bands x that, learned apart: booked as pixel cost, the repeat would price the state's next draw longer and band it
// more (25 Nov: VectorKit's ground draws of 48-208 instructions took 8-17 ms in Maps at 1.0-2.9 ms a band, where their
// pixels cost 0.06-0.8 ms). A banded draw teaches the band's cost, what its time holds past its pixels' (the learned
// rate's, else the static price); a whole draw teaches the pixels' rate, what its time holds past one band's repeat.
inline void learn(BandRate &r, uint64_t ns, uint64_t units, uint32_t bands = 1, uint64_t staticPixelNs = 0)
{
    if (!units || !ns)
        return;
    if (bands > 1) {
        const uint64_t ps = r.psPerUnit.load(std::memory_order_relaxed);
        const uint64_t pixelNs = ps ? unitsNs(units, ps) : staticPixelNs;
        const uint64_t sample = std::min<uint64_t>((ns > pixelNs ? ns - pixelNs : 0) / bands, UINT32_MAX);
        const uint64_t old = r.nsPerBand.load(std::memory_order_relaxed);
        const uint64_t perBand = !old ? sample : (old * (kRateWeight - 1) + sample) / kRateWeight;
        r.nsPerBand.store(uint32_t(std::max<uint64_t>(perBand, 1)), std::memory_order_relaxed);
        r.samples.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint64_t repeat = r.nsPerBand.load(std::memory_order_relaxed);
    ns = ns > repeat ? ns - repeat : 1;
    const uint64_t sample = std::clamp<uint64_t>(ns * 1000 / units, 1, UINT32_MAX);
    const uint64_t old = r.psPerUnit.load(std::memory_order_relaxed);
    const uint64_t rate = !old || sample > old ? sample : (old * (kRateWeight - 1) + sample) / kRateWeight;
    r.psPerUnit.store(uint32_t(std::max<uint64_t>(rate, 1)), std::memory_order_relaxed);
    r.samples.fetch_add(1, std::memory_order_relaxed);
}

// A measured draw's record, kept by its command buffer until completion: the rate it teaches, its stamp slot and payload,
// and for a debug build's long draw line its state's key, program instructions and bands
struct DrawTiming {
    BandRate *rate = nullptr;
    uint64_t units = 0;
    uint32_t slot = 0, payload = 0;
    uint64_t key = 0;
    uint32_t instructions = 0, bands = 1;
};

constexpr uint64_t kLongDrawNs = 5'000'000;

}  // namespace gm20b
