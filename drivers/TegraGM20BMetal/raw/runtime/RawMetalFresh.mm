// SPDX-License-Identifier: MIT
#import "RawMetalFresh.h"
#include "raw_fresh.h"
#include "raw_fresh_queue.h"

namespace {

struct FreshEntry {
    __weak RMTexture *texture;
};

raw::FreshQueue<FreshEntry> gFresh;
std::atomic<uint64_t> gFilled{0}, gFilledBytes{0}, gCleared{0}, gAhead{0};

#if SWITCH_RAW_DIAGNOSTICS
std::mutex gTraceLock;
NSMapTable<RMTexture *, NSString *>
    *gUnwritten;  // fresh textures no command wrote yet: the read before it, if one came
std::atomic<uint64_t> gEarlyReads{0};
#endif

RMTexture *ownerOf(RMTexture *t)
{
    while (t && t->_kind == RMTextureView && t->_parent)
        t = t->_parent;
    return t;
}

void fillLevel(raw::BlitEncoder &b, const raw::Texture &image, const raw::CopyFill &fill, uint32_t planes)
{
    for (uint32_t z = 0; z < planes; z++)
        b.fillTexels(image, z, image.width, image.height, fill);
}

uint64_t writeFresh(raw::BlitEncoder &b, RMTexture *t)
{
    MTLTextureDescriptor *d = t->_descriptor;
    const raw::Texture &x = t->_raw;
    const uint32_t levels = x.chain ? x.chain->layout.levels : 1;
    const uint32_t patterned = raw::patternLevels(uint64_t(d.usage), d.allowGPUOptimizedContents, uint32_t(d.width),
                                                  uint32_t(d.height), levels);
    const uint8_t zeros[16] = {};
    raw::CopyFill zero, pattern;
    raw::FreshTexel texel;
    if (!raw::copyFillOf(zeros, x.bpp, zero))
        return 0;
    const bool hasPattern = patterned && raw::freshPatternTexel(x.format, texel) && texel.size == x.bpp &&
                            raw::copyFillOf(texel.bytes, texel.size, pattern);
    const uint32_t planes = x.type == raw::fx::kTexThreeD ? x.depth : x.layers;
    uint64_t bytes = 0;
    for (uint32_t level = 0; level < levels; level++) {
        raw::Texture image;
        if (x.chain && !raw::chainLevelImage(x, level, image))
            continue;
        const raw::Texture &target = x.chain ? image : x;
        fillLevel(b, target, hasPattern && level < patterned ? pattern : zero, planes);
        bytes += uint64_t(target.width) * target.height * planes * x.bpp;
    }
    return bytes;
}

enum class FirstUse {
    None,
    ClearsAll,
    Other
};

// The first encoder of [from, to) that names `t` (or a view of it): a pass that clears and stores all of it before
// anything reads it (raw_fresh.h firstPassWritesAll), or any other use
FirstUse firstUse(RMCommandBuffer *cb, size_t from, size_t to, RMTexture *t)
{
    for (size_t at = from; at < to;) {
        const auto op = cb->_plan.operation(at).op;
        if (op != OpRender && op != OpCompute && op != OpBlit) {
            at++;
            continue;
        }
        const size_t end = cb->_plan.encoderEnd(at);
        uint8_t access = 0;
        bool named = false;
        cb->_plan.forEachResource(at, end, RMResourceKind::Texture, [&](const RMRangeResource &use) {
            if (ownerOf((__bridge RMTexture *)use.object) == t) {
                named = true;
                access |= use.access;
            }
        });
        if (!named) {
            at = std::max(end, at + 1);
            continue;
        }
        if (op != OpRender)
            return FirstUse::Other;
        const RMPass &pass = cb->_passes[cb->_plan.legacy(at).x];
        const raw::Texture &x = t->_raw;
        for (const RMPass::Color &c : pass.color)
            if (c.texture == t && c.level == 0 &&
                raw::firstPassWritesAll(x.width, x.height, x.chain ? x.chain->layout.levels : 1,
                                        x.type == raw::fx::kTexThreeD ? x.depth : x.layers,
                                        c.load == MTLLoadActionClear, c.store == MTLStoreActionStore,
                                        access & raw::ResourceUse::Sample, uint32_t(pass.width), uint32_t(pass.height)))
                return FirstUse::ClearsAll;
        return FirstUse::Other;
    }
    return FirstUse::None;
}

}  // namespace

void establishFresh(RMTexture *t, MTLTextureDescriptor *d)
{
    if (!t->_rawOk || t->_kind != RMTexturePool || t->_heap || t->_raw.memoryless || t->_raw.arena || t->_raw.zeta ||
        d.storageMode != MTLStorageModePrivate)
        return;
    gFresh.add({t});
#if SWITCH_RAW_DIAGNOSTICS
    std::lock_guard<std::mutex> l(gTraceLock);
    if (!gUnwritten)
        gUnwritten = [NSMapTable weakToStrongObjectsMapTable];
    [gUnwritten setObject:@"" forKey:t];
#endif
}

FreshWrites::FreshWrites(RMCommandBuffer *cb, size_t from, size_t to, raw::CommandBuffer &rc)
{
    std::vector<FreshEntry> taken = gFresh.take(), later;
    raw::BlitEncoder *b = nullptr;
    for (FreshEntry &e : taken) {
        RMTexture *t = e.texture;
        if (!t || t->_heap)
            continue;
        const FirstUse use = to > from ? firstUse(cb, from, to, t) : FirstUse::Other;
        if (use == FirstUse::None) {
            later.push_back({t});
            continue;
        }
        handled_.push_back(t);
        if (use == FirstUse::ClearsAll) {
            gCleared.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (!b)
            b = rc.blit();
        gFilledBytes.fetch_add(writeFresh(*b, t), std::memory_order_relaxed);
        gFilled.fetch_add(1, std::memory_order_relaxed);
    }
    if (b) {
        b->barrier();
        b->end();
    }
    gFresh.settle(std::move(later), false);
}

void FreshWrites::submitted(RMCommandBuffer *cb)
{
    submitted_ = true;
    cb->_owners.insert(cb->_owners.end(), handled_.begin(), handled_.end());
}

FreshWrites::~FreshWrites()
{
    if (submitted_ || handled_.empty())
        return;
    std::vector<FreshEntry> back;
    for (id t : handled_)
        back.push_back({(RMTexture *)t});
    gFresh.settle(std::move(back), false);
}

bool writeFreshAhead(RMCommandBuffer *cb, size_t from, size_t to)
{
#if SWITCH_RAW_DIAGNOSTICS
    for (size_t at = from; at < to;) {
        const auto op = cb->_plan.operation(at).op;
        if (op != OpRender && op != OpCompute && op != OpBlit) {
            at++;
            continue;
        }
        traceFreshUses(cb, at);
        at = std::max(cb->_plan.encoderEnd(at), at + 1);
    }
#else
    (void)from;
    (void)to;
#endif
    if (!gFresh.any())
        return true;
    raw::CommandBuffer *rc = gRM->raw->begin();
    if (!rc)
        return false;
    rc->internalCompletion();
    FreshWrites writes(cb, 0, 0, *rc);
    uint32_t seqno = 0;
    if (!rc->commit(seqno)) {
        rc->abandon();
        return false;
    }
    writes.submitted(cb);
    gRM->rawSinceM2v = true;
    gAhead.fetch_add(1, std::memory_order_relaxed);
    return true;
}

#if SWITCH_RAW_DIAGNOSTICS
void traceFreshUses(RMCommandBuffer *cb, size_t begin)
{
    std::lock_guard<std::mutex> l(gTraceLock);
    if (!gUnwritten.count)
        return;
    const size_t end = cb->_plan.encoderEnd(begin);
    NSString *here = nil;
    auto describe = [&] {
        if (here)
            return here;
        const auto op = cb->_plan.operation(begin).op;
        NSString *state = @"-";
        if (op == OpRender)
            for (void *p : cb->_plan.renderPipelines(begin, end))
                if (NSString *label = ((__bridge RMPipeline *)p).label) {
                    state = label;
                    break;
                }
        here = [NSString stringWithFormat:@"%s %zu of command buffer %p (%@)",
                                          op == OpRender ? "pass"
                                          : op == OpBlit ? "blit"
                                                         : "compute",
                                          begin, cb, state];
        return here;
    };
    cb->_plan.forEachResource(begin, end, RMResourceKind::Texture, [&](const RMRangeResource &use) {
        RMTexture *t = ownerOf((__bridge RMTexture *)use.object);
        NSString *readBy = [gUnwritten objectForKey:t];
        if (!readBy)
            return;
        if (t->_heap) {
            [gUnwritten removeObjectForKey:t];
            return;
        }
        const raw::Texture &x = t->_raw;
        const bool reads = (use.access & raw::ResourceUse::Sample) ||
                           ((use.access & raw::ResourceUse::Read) && !(use.access & raw::ResourceUse::Write));
        if (reads && !readBy.length) {
            const uint64_t n = gEarlyReads.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 64 || n % 256 == 0)
                os_log(rawLog(),
                       "raw: fresh %ux%u format %u texture %{public}@ read by %{public}@ before any command wrote it "
                       "(#%llu)",
                       x.width, x.height, x.format, t.label ?: @"-", describe(), (unsigned long long)n);
            readBy = describe();
            [gUnwritten setObject:readBy forKey:t];
        }
        if (!(use.access & raw::ResourceUse::Write))
            return;
        if (readBy.length)
            os_log(
                rawLog(),
                "raw: fresh %ux%u format %u texture %{public}@ first written by %{public}@, after %{public}@ read it",
                x.width, x.height, x.format, t.label ?: @"-", describe(), readBy);
        [gUnwritten removeObjectForKey:t];
    });
}
#endif

@implementation RMDevice (Fresh)

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// hw_test_app's gpu.texture.fresh: textures written to their first contents by the copy engine, their bytes, those a first
// pass cleared whole instead, submissions made ahead of the other driver's for them, and reads of a fresh texture before any
// command wrote it
- (NSDictionary<NSString *, NSNumber *> *)switchRawFresh
{
    NSMutableDictionary *d = [NSMutableDictionary dictionary];
    d[@"filled"] = @(gFilled.load());
    d[@"filled_bytes"] = @(gFilledBytes.load());
    d[@"cleared"] = @(gCleared.load());
    d[@"ahead"] = @(gAhead.load());
#if SWITCH_RAW_DIAGNOSTICS
    d[@"early_reads"] = @(gEarlyReads.load());
#endif
    return d;
}
#endif

@end
