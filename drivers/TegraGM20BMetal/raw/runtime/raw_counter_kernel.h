// SPDX-License-Identifier: MIT
// The blit encoder's resolveCounters: on the GPU: a kernel the raw device compiles for itself (RawMetalCounterEncode.mm),
// each thread one record into one sample in nanoseconds, as raw_counters.h resolved() does on the CPU. It works in 32-bit
// words (no 64-bit integer kernel is proven through the runtime's compile path): the span from the anchor in 16-bit
// digits, divided by 384 and multiplied by 625 a digit at a time. resolvedInWords is the same arithmetic on the host, which raw_counters_test holds
// equal to resolved().
#pragma once
#include <cstdint>
#include "raw_counters.h"

namespace raw::counters {

inline constexpr const char *kResolveFunction = "switch_counter_resolve";

inline constexpr const char *kResolveSource = R"(#include <metal_stdlib>
using namespace metal;

struct SwitchCounterResolve {
    uint anchor[4];
    uint count;
    uint valid;
};

kernel void switch_counter_resolve(device const uint4 *records [[buffer(0)]], device uint *samples [[buffer(1)]],
                                   constant SwitchCounterResolve &p [[buffer(2)]], uint i [[thread_position_in_grid]])
{
    if (i >= p.count)
        return;
    const uint4 r = records[i];
    uint lo = 0, hi = 0;
    if (r.x != 0 && p.valid == 0) {
        lo = 0xffffffffu;
        hi = 0xffffffffu;
    } else if (r.x != 0) {
        uint dLo = r.z - p.anchor[0];
        uint dHi = r.w - p.anchor[1] - (r.z < p.anchor[0] ? 1u : 0u);
        const bool negative = (dHi >> 31) != 0;
        if (negative) {
            dLo = ~dLo + 1u;
            dHi = ~dHi + (dLo == 0u ? 1u : 0u);
        }
        const uint digits[4] = {dHi >> 16, dHi & 0xffffu, dLo >> 16, dLo & 0xffffu};
        uint q[4];
        uint rem = 0;
        for (uint k = 0; k < 4; k++) {
            const uint cur = (rem << 16) | digits[k];
            q[k] = cur / 384u;
            rem = cur % 384u;
        }
        uint m[4];
        uint carry = 0;
        for (uint k = 4; k-- > 0;) {
            const uint v = q[k] * 625u + carry;
            m[k] = v & 0xffffu;
            carry = v >> 16;
        }
        uint mHi = (m[0] << 16) | m[1], mLo = (m[2] << 16) | m[3];
        const uint frac = (625u * rem + (negative ? 383u : 0u)) / 384u;
        const uint t = mLo + frac;
        mHi += t < mLo ? 1u : 0u;
        mLo = t;
        if (!negative) {
            lo = p.anchor[2] + mLo;
            hi = p.anchor[3] + mHi + (lo < p.anchor[2] ? 1u : 0u);
        } else {
            lo = p.anchor[2] - mLo;
            hi = p.anchor[3] - mHi - (p.anchor[2] < mLo ? 1u : 0u);
        }
    }
    samples[2 * i] = lo;
    samples[2 * i + 1] = hi;
}
)";

constexpr uint32_t kResolveThreads = 64;

// SwitchCounterResolve: the anchor's units and nanoseconds (low word first), the samples, whether the anchor is one
struct ResolveParams {
    uint32_t anchor[4];
    uint32_t count, valid;
};

static_assert(sizeof(ResolveParams) == 24, "SwitchCounterResolve");

inline ResolveParams resolveParams(const Anchor &a, uint32_t count)
{
    return {{uint32_t(a.units), uint32_t(a.units >> 32), uint32_t(a.ns), uint32_t(a.ns >> 32)}, count, a.valid};
}

inline uint64_t resolvedInWords(const Record &r, const ResolveParams &p)
{
    if (!r.payload)
        return 0;
    if (!p.valid)
        return kErrorValue;
    const uint32_t unitsLo = uint32_t(r.units), unitsHi = uint32_t(r.units >> 32);
    uint32_t dLo = unitsLo - p.anchor[0];
    uint32_t dHi = unitsHi - p.anchor[1] - (unitsLo < p.anchor[0] ? 1u : 0u);
    const bool negative = (dHi >> 31) != 0;
    if (negative) {
        dLo = ~dLo + 1u;
        dHi = ~dHi + (dLo == 0u ? 1u : 0u);
    }
    const uint32_t digits[4] = {dHi >> 16, dHi & 0xffffu, dLo >> 16, dLo & 0xffffu};
    uint32_t q[4];
    uint32_t rem = 0;
    for (uint32_t k = 0; k < 4; k++) {
        const uint32_t cur = (rem << 16) | digits[k];
        q[k] = cur / 384u;
        rem = cur % 384u;
    }
    uint32_t m[4];
    uint32_t carry = 0;
    for (uint32_t k = 4; k-- > 0;) {
        const uint32_t v = q[k] * 625u + carry;
        m[k] = v & 0xffffu;
        carry = v >> 16;
    }
    uint32_t mHi = (m[0] << 16) | m[1], mLo = (m[2] << 16) | m[3];
    const uint32_t frac = (625u * rem + (negative ? 383u : 0u)) / 384u;
    const uint32_t t = mLo + frac;
    mHi += t < mLo ? 1u : 0u;
    mLo = t;
    uint32_t lo, hi;
    if (!negative) {
        lo = p.anchor[2] + mLo;
        hi = p.anchor[3] + mHi + (lo < p.anchor[2] ? 1u : 0u);
    } else {
        lo = p.anchor[2] - mLo;
        hi = p.anchor[3] - mHi - (p.anchor[2] < mLo ? 1u : 0u);
    }
    return uint64_t(hi) << 32 | lo;
}

}  // namespace raw::counters
