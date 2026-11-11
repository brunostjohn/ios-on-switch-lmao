// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"
#include "raw_push.h"

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// GM20B methods Metal cannot reach, for hw_test_app's hardware probes (app27/X1RawProbes.swift): each NSData holds
// (method, value) uint32 pairs of the 3D class. Every draw of one instance this process encodes emits `before` after
// its state and `after` once it ended, until a call with `before` nil disarms them and answers how many draws emitted
// them. The probe's command buffers must complete before it disarms: their encoding may run after commit returns.
static bool appendMethods(NSData *pairs, std::vector<uint32_t> &words)
{
    if (!pairs)
        return true;
    if (pairs.length % 8)
        return false;
    const auto *v = static_cast<const uint32_t *>(pairs.bytes);
    for (NSUInteger i = 0; i < pairs.length / 4; i += 2) {
        if (v[i] >= 0x8000 || v[i] % 4)
            return false;
        words.push_back(raw::Push::hdr(1, raw::SUBC_3D, v[i], 1));
        words.push_back(v[i + 1]);
    }
    return true;
}

@implementation RMDevice (Probe)

// Arming: nil, else why not. Disarming: the draws that emitted them, as a decimal string.
- (NSString *)switchRawProbeBefore:(NSData *)before after:(NSData *)after
{
    raw::Device &d = *gRM->raw;
    if (!before) {
        d.probeArmed.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> hold(d.probeLock);
        d.probeWords.reset();
        return [NSString stringWithFormat:@"%llu", (unsigned long long)d.probeEmitted.exchange(0)];
    }
    auto words = std::make_shared<raw::Device::ProbeWords>();
    if (!appendMethods(before, words->before) || !appendMethods(after, words->after))
        return @"methods are (method, value) uint32 pairs of 3D-class methods below 0x8000";
    if (words->before.size() + words->after.size() > 1024)
        return @"more than 512 methods";
    {
        std::lock_guard<std::mutex> hold(d.probeLock);
        d.probeWords = std::move(words);
        d.probeEmitted.store(0, std::memory_order_relaxed);
    }
    d.probeArmed.store(true, std::memory_order_release);
    return nil;
}

@end
#endif
