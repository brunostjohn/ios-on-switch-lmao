// SPDX-License-Identifier: MIT
// What a state's draws (or a kernel's dispatches) cost the GPU, learned from GPU time stamps of its own large draws, which the
// band decision (raw_draw_bands.h, raw_dispatch_bands.h) reads in place of the instruction-count estimate once it has one.
// The static estimate prices a draw as pixels x program instructions, so a draw bound by memory or the ROP (a 4x resolve:
// 642 instructions, priced 3 ms, measured 11.5-12 ms) or a looped program is priced far short and kept whole.
//
// A unit is a pixel of the draw's box times its sample passes and instances (a thread of a dispatch). The first kWarmSamples
// draws of at least kMeasureUnits units, then one in kRemeasureEvery, are measured: a stamp before and one after, read at
// the command buffer's completion. The table is per state, its reads and its update relaxed atomics: a racing update loses
// a sample, never a rate.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>

namespace raw {

constexpr uint64_t kMeasureUnits = 64 * 1024;
constexpr uint32_t kWarmSamples = 3;
constexpr uint32_t kRemeasureEvery = 32;
constexpr uint32_t kMeasuredPerCommandBuffer = 16;
constexpr uint32_t kRateWeight = 4;  // a new sample moves a warm rate by a quarter of the difference

struct BandRate {
    std::atomic<uint32_t> psPerUnit{0};  // 0: not measured yet
    std::atomic<uint32_t> samples{0};
    std::atomic<uint32_t> asked{0};

    BandRate() = default;

    BandRate(const BandRate &o)
    {
        *this = o;
    }

    BandRate &operator=(const BandRate &o)
    {
        psPerUnit.store(o.psPerUnit.load(std::memory_order_relaxed), std::memory_order_relaxed);
        samples.store(o.samples.load(std::memory_order_relaxed), std::memory_order_relaxed);
        asked.store(o.asked.load(std::memory_order_relaxed), std::memory_order_relaxed);
        return *this;
    }
};

// The work's GPU time: `units` at the learned rate, else the static estimate
inline uint64_t estimatedNs(const BandRate &r, uint64_t units, uint64_t staticNs)
{
    const uint64_t ps = r.psPerUnit.load(std::memory_order_relaxed);
    if (!ps)
        return staticNs;
    return units > UINT64_MAX / ps ? UINT64_MAX / 1000 : units * ps / 1000;
}

// Whether this draw of `units` is one to measure
inline bool measure(BandRate &r, uint64_t units)
{
    if (units < kMeasureUnits)
        return false;
    if (r.samples.load(std::memory_order_relaxed) < kWarmSamples)
        return true;
    return r.asked.fetch_add(1, std::memory_order_relaxed) % kRemeasureEvery == 0;
}

// A measured draw: `ns` of GPU time over `units`
inline void learn(BandRate &r, uint64_t ns, uint64_t units)
{
    if (!units || !ns)
        return;
    const uint64_t sample = std::clamp<uint64_t>(ns * 1000 / units, 1, UINT32_MAX);
    const uint64_t old = r.psPerUnit.load(std::memory_order_relaxed);
    const uint64_t rate = old ? (old * (kRateWeight - 1) + sample) / kRateWeight : sample;
    r.psPerUnit.store(uint32_t(std::max<uint64_t>(rate, 1)), std::memory_order_relaxed);
    r.samples.fetch_add(1, std::memory_order_relaxed);
}

// A measured draw's record, kept by its command buffer until completion: the rate it teaches, its stamp slot and payload,
// and for a debug build's long draw line its state's key and program instructions
struct DrawTiming {
    BandRate *rate = nullptr;
    uint64_t units = 0;
    uint32_t slot = 0, payload = 0;
    uint64_t key = 0;
    uint32_t instructions = 0;
};

constexpr uint64_t kLongDrawNs = 5'000'000;

}  // namespace raw
