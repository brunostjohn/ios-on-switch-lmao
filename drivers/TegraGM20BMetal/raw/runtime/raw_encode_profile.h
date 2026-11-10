// SPDX-License-Identifier: MIT
// Debug builds: where the submit worker's encode of a raw range spends its time (the rates line's enc=), by part, for
// the plugin's "encode" line. A part's time includes the parts nested in it: Resources, Loop and Tail are the encode's
// three phases; Loop holds State, Pass (which holds SplitPlan), Fusion, Blit and Compute; Commit, in Tail, holds
// Prologues. Draw (which holds Prepare, and Prepare Geometry, Descriptors, Barriers and SplitLaunch), PassEnd, Kernel,
// GeometryAcquire and GeometryRelease are counted in whichever phase they run. A span outside an encode (another
// thread, or the submit worker between encodes) costs one thread-local read.
#pragma once
#include <cstddef>
#include <cstdint>

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
#include <mach/mach_time.h>
#include <atomic>

namespace raw {

enum class EncodePart : uint8_t {
    Resources,
    Loop,
    State,
    Pass,
    SplitPlan,
    Fusion,
    PassEnd,
    Draw,
    Prepare,
    Geometry,
    Descriptors,
    Barriers,
    SplitLaunch,
    Blit,
    Compute,
    Tail,
    Commit,
    Prologues,
    Kernel,
    GeometryAcquire,
    GeometryRelease,
    Count
};

constexpr size_t kEncodeParts = size_t(EncodePart::Count);

struct EncodeProfile {
    uint64_t ticks[kEncodeParts] = {};
    uint64_t calls[kEncodeParts] = {};
    uint64_t longestPrepare = 0;
};

inline thread_local EncodeProfile *tEncodeProfile = nullptr;

class EncodeSpan {
public:
    explicit EncodeSpan(EncodePart part)
        : profile_(tEncodeProfile), part_(part), began_(profile_ ? mach_absolute_time() : 0)
    {}

    ~EncodeSpan()
    {
        stop();
    }

    EncodeSpan(const EncodeSpan &) = delete;
    EncodeSpan &operator=(const EncodeSpan &) = delete;

    void stop()
    {
        if (!profile_)
            return;
        const uint64_t ticks = mach_absolute_time() - began_;
        profile_->ticks[size_t(part_)] += ticks;
        profile_->calls[size_t(part_)]++;
        if (part_ == EncodePart::Prepare && ticks > profile_->longestPrepare)
            profile_->longestPrepare = ticks;
        profile_ = nullptr;
    }

private:
    EncodeProfile *profile_;
    EncodePart part_;
    uint64_t began_;
};

// The encodes' profiles since the device opened, each folded in once its encode returned (the submit worker's only)
struct EncodeTotals {
    std::atomic<uint64_t> encodes{0}, wallTicks{0}, cpuNs{0}, longestPrepare{0};
    std::atomic<uint64_t> ticks[kEncodeParts] = {}, calls[kEncodeParts] = {};

    void add(const EncodeProfile &p, uint64_t wall, uint64_t cpu)
    {
        encodes.fetch_add(1, std::memory_order_relaxed);
        wallTicks.fetch_add(wall, std::memory_order_relaxed);
        cpuNs.fetch_add(cpu, std::memory_order_relaxed);
        for (size_t i = 0; i < kEncodeParts; ++i) {
            if (!p.calls[i])
                continue;
            ticks[i].fetch_add(p.ticks[i], std::memory_order_relaxed);
            calls[i].fetch_add(p.calls[i], std::memory_order_relaxed);
        }
        uint64_t seen = longestPrepare.load(std::memory_order_relaxed);
        while (p.longestPrepare > seen &&
               !longestPrepare.compare_exchange_weak(seen, p.longestPrepare, std::memory_order_relaxed)) {
        }
    }
};

}  // namespace raw

#define RAW_ENCODE_SPAN_JOIN2(a, b)  a##b
#define RAW_ENCODE_SPAN_JOIN(a, b)   RAW_ENCODE_SPAN_JOIN2(a, b)
#define RAW_ENCODE_SPAN(part)        const raw::EncodeSpan RAW_ENCODE_SPAN_JOIN(rawEncodeSpan, __LINE__)(raw::EncodePart::part)
#define RAW_ENCODE_BEGIN(name, part) raw::EncodeSpan name(raw::EncodePart::part)
#define RAW_ENCODE_END(name)         name.stop()
#else
#define RAW_ENCODE_SPAN(part)        static_cast<void>(0)
#define RAW_ENCODE_BEGIN(name, part) static_cast<void>(0)
#define RAW_ENCODE_END(name)         static_cast<void>(0)
#endif
