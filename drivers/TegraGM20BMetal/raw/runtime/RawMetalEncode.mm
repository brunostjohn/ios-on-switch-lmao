// SPDX-License-Identifier: MIT
#import "RawMetalPlanAdapters.h"
#import "RawMetalPassFusion.h"
#import "RawMetalSplitPlan.h"
#include <optional>
#include "raw_compute.h"
#include "raw_compute_lookup.h"
#include "raw_samples.h"

// ---- the tape onto the raw path ----
// false: something in it the raw path cannot do after all (*why); nothing of it was submitted then. A refusal inside one
// encoder names that encoder's first command in *refusedEncoder; one no encoder owns (the channel's, the kernel's, a planar
// texture sampled and rendered across the range) leaves it alone.
// The host imports a texture's memory lies in, named to the submission (raw::CommandBuffer::use): a surface's or a planar
// texture's planes, a buffer texture's buffer's, a view's parent's; and a YCbCr texture's planes, which only its conversion
// reads (its image is pool memory).
void useTexture(raw::CommandBuffer *rc, RMTexture *t, bool write)
{
    while (t && t->_kind == RMTextureView && t->_parent)
        t = t->_parent;
    if (!t)
        return;
    rc->use(t->_raw, write);
    if (t->_buffer)
        rc->use(t->_buffer->_raw, write);
    for (const raw::Texture *plane : {t->_raw.luma, t->_raw.chroma})
        if (plane)
            rc->use(*plane, false);
}

void useBuffer(raw::CommandBuffer *rc, RMBuffer *b, bool write)
{
    if (b)
        rc->use(b->_raw, write);
}

static bool encodeRawAttempt(RMCommandBuffer *cb, size_t from, size_t to, const char **why, bool &stale,
                             size_t *refusedEncoder)
{
    raw::Device &d = *gRM->raw;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    d.nextForms = cb->_probeForms;
#endif
    raw::CommandBuffer *rc = d.begin();
    if (!rc) {
        *why = "the kernel gave the raw path no channel";
        if (!predLogged(PredEncNoChannel))
            logPred(PredEncNoChannel, 0, 0, @(d.channelError().c_str()));
        hit(cb, PredEncNoChannel);
        return false;
    }
    const uint64_t generation = d.channelGeneration();
    rc->internalCompletion();
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    rc->forceWholePrograms(cb->_forceWholePrograms);
#endif
    if (!validateNativeEventWaits(cb, generation)) {
        rc->abandon();
        *why = "the event producer no longer belongs to this GPU channel";
        return false;
    }
    RAW_ENCODE_BEGIN(resourceSpan, Resources);
    const auto buffers = [&](auto visit) {
        cb->_plan.forEachResource(from, to, RMResourceKind::Buffer, visit);
    };
    const auto textures = [&](auto visit) {
        cb->_plan.forEachResource(from, to, RMResourceKind::Texture, visit);
    };
    const auto pitchBuffer = [&](const RMRangeResource &use) {
        if (RMBuffer *buffer = (__bridge RMBuffer *)use.facts.cpuBuffer)
            rc->requirePitch(buffer->_raw.import);
    };
    buffers(pitchBuffer);
    textures([&](const RMRangeResource &use) {
        pitchBuffer(use);
        RMTexture *texture = (__bridge RMTexture *)use.object;
        if ((use.access & (raw::ResourceUse::Write | raw::ResourceUse::Explicit)) ||
            !(use.access & raw::ResourceUse::Sample)) {
            rc->requirePitch(texture->_raw.import);
            for (const raw::Texture *plane : {texture->_raw.luma, texture->_raw.chroma})
                if (plane)
                    rc->requirePitch(plane->import);
        }
    });
    textures([&](const RMRangeResource &use) {
        if (!(use.access & raw::ResourceUse::Sample))
            return;
        RMTexture *texture = (__bridge RMTexture *)use.object;
        for (const raw::Texture *plane : {&texture->_raw, texture->_raw.luma, texture->_raw.chroma})
            if (plane && plane->nativePlane != ~0u && rc->readTexture(plane) == plane)
                rc->requirePitch(plane->import);
    });
    const auto cleanBuffer = [&](const RMRangeResource &use) {
        const bool write = use.access & raw::ResourceUse::Write;
        if (RMBuffer *buffer = (__bridge RMBuffer *)use.facts.cpuBuffer;
            buffer && buffer->_raw.hostVisible && !buffer->_raw.wc &&
            (write || (use.access & raw::ResourceUse::Explicit)))
            rc->clean(buffer->_raw.cpu, buffer->_raw.length, write);
    };
    buffers([&](const RMRangeResource &use) {
        cleanBuffer(use);
        useBuffer(rc, (__bridge RMBuffer *)use.object, use.access & raw::ResourceUse::Write);
    });
    textures([&](const RMRangeResource &use) {
        cleanBuffer(use);
        useTexture(rc, (__bridge RMTexture *)use.object, use.access & raw::ResourceUse::Write);
    });
    raw::RenderEncoder *re = nullptr;
    raw::BlitEncoder *be = nullptr;
    std::optional<raw::ComputeEncoder> ce;
    bool serialCompute = false;
    raw::StateCursor stateCursor;
    size_t renderBegin = to;
    bool continuing = false;
    size_t encoder = to;

    auto fail = [&](RMPred p, const char *w, uint64_t a = 0, uint64_t b = 0) {
        *why = w;
        hit(cb, p, a, b);
        if (encoder < to)
            *refusedEncoder = encoder;
        rc->abandon();
        return false;
    };
    // a draw the encoder refused: its predicate and values (raw_gpu.h Refusal)
    auto refused = [&]() {
        static const RMPred map[] = {PredEncPassRender,  PredEncIndexedRange, PredEncColorFormats, PredEncUboOffset,
                                     PredEncNoTic,       PredEncDepthStencil, PredEncSamples,      PredEncGeometry,
                                     PredEncStorageView, PredPassRotated};
        const raw::Refusal &f = rc->refusal;
        hit(cb, f.id < sizeof map / sizeof *map ? map[f.id] : PredEncPassRender, f.a,
            uint64_t(f.b) | uint64_t(f.c) << 32);
        if (encoder < to)
            *refusedEncoder = encoder;
        rc->abandon();
        return false;
    };
    // A planar render target's stand-in passes (raw::Texture::planarStandIn), the frontend's: the planes' codes into the stand-in
    // before a pass that loads it (its code image from the planes first, the copy engine's remaps), the stand-in into the planes
    // after one that stores it. 0: drawn; else fail()'s or refused()'s answer.
    std::vector<RMTexture *> standInLoads, standInStores, planesWritten;
    auto standInPass = [&](RMTexture *t, bool store) -> int {
        raw::Texture &x = t->_raw;
        if (!store) {
            raw::BlitEncoder *b = rc->blit();
            b->convertPlanar(x);
            b->end();
        }
        raw::PassDesc p;
        p.color[0].texture = store ? x.luma : x.planarStandIn;
        p.color[1].texture = store ? x.chroma : nullptr;
        for (raw::ColorAttachment &a : p.color) {
            a.load = raw::ColorAttachment::DontCare;
            a.store = true;
        }
        p.count = store ? 2 : 1;
        raw::RenderEncoder *e = rc->render(p);
        if (!e) {
            fail(PredEncPlanarRT, "a planar texture's stand-in pass the raw path cannot render", store);
            return 1;
        }
        e->setPipeline(d.standInPipeline(store));
        e->setTexture(0, store ? x.planarStandIn : &x);
        if (!e->draw(3 /* MTLPrimitiveTypeTriangle */, 0, 3, 1, 0, why)) {
            refused();
            return 2;
        }
        e->end();
        if (store)
            planesWritten.push_back(t);
        return 0;
    };

    // Multisampled attachments' resolves (MTLStoreActionMultisampleResolve): after their pass, a pass of the pack's resolve state
    // of the format each, into the resolve texture over the pass's render area (an 8-bit sRGB one as its UNORM twin, whose codes
    // the state writes), sampling what the pass rendered (the resolve pass's prologue barrier orders its reads after those
    // writes)
    struct Resolve {
        uint32_t attachment;
        RMTexture *into;
    };

    std::vector<Resolve> resolves;
    std::deque<raw::Texture> resolveTargets;  // the encoder reads them until its end()
    auto resolvePasses = [&]() -> bool {
        const raw::Texture *from[8] = {};
        for (const Resolve &r : resolves)
            from[r.attachment] = re->target(r.attachment);
        const uint32_t width = re->areaWidth(), height = re->areaHeight();
        for (const Resolve &r : resolves) {
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
            if (rc->probeForms().resolveTwoD) {
                useTexture(rc, r.into, true);
                if (rc->probeResolve(*from[r.attachment], r.into->_raw))
                    continue;
            }
#endif
            resolveTargets.push_back(r.into->_raw);
            resolveTargets.back().format = raw::resolveTargetFormat(r.into->_raw.format);
            raw::PassDesc p;
            p.color[0].texture = &resolveTargets.back();
            p.color[0].load = raw::ColorAttachment::DontCare;
            p.color[0].store = true;
            p.count = 1;
            p.width = width;
            p.height = height;
            useTexture(rc, r.into, true);
            raw::RenderEncoder *e = rc->render(p);
            if (!e)
                return fail(PredEncPassRender, "a resolve texture the raw path cannot render", r.attachment);
            const uint32_t samples = from[r.attachment]->samples;
            e->setPipeline(d.resolvePipeline(r.into->_raw.format));
            e->setTexture(0, from[r.attachment]);
            e->setBytes(2, 0, &samples, sizeof samples);
            if (!e->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why))
                return refused();
            e->end();
        }
        resolves.clear();
        return true;
    };

    // the planar textures its draws sample: their images from their planes first, once (m2v converts at each encoder that uses
    // one; nothing between this command buffer's passes writes the planes, refused below), then the passes, whose prologue's
    // full barrier orders the copies before their reads. A texture bound where no draw's state reads it is not converted: the
    // image is made when a draw samples it. A sampler that clamps to zero or to the border reads a 4:4:4 code texture as clamp to
    // edge (m2v's address map), which the raw path does not derive: such a command buffer is m2v's.
    struct Conversion {
        RMTexture *t;
        uint32_t seed;
        uint64_t writes, nativeGeneration;
    };

    std::vector<Conversion> converted;  // planar images converted here, at their surfaces' seeds and write counts
    {
        std::vector<RMTexture *> planar;
        std::vector<RMSampler *> samplers;
        cb->_plan.forEachResource(from, to, RMResourceKind::Sampler, [&](const RMRangeResource &use) {
            samplers.push_back((__bridge RMSampler *)use.object);
        });
        textures([&](const RMRangeResource &use) {
            if ((use.access & raw::ResourceUse::Sample) && (use.facts.roles & RMResourceFacts::Converted))
                planar.push_back((__bridge RMTexture *)use.object);
        });
        if (!planar.empty()) {
            // the samplers' headers under the planar textures' address maps (m2v's), which the draws' handles take
            for (RMTexture *t : planar)
                for (RMSampler *sm : samplers)
                    if (t->_raw.addressMap && !d.samplerVariant(sm->_raw, t->_raw.addressMap))
                        return fail(PredEncPlanarClamp, "no sampler header for an address map");
            // an image converted already from its surface at the surface's seed now is not converted again: the work that
            // converted it went before on this channel (the decoder and ImageIO write a surface under a write lock, which
            // moves its seed). The new seeds take effect once this command buffer is submitted.
            std::vector<RMTexture *> stale;
            for (RMTexture *t : planar) {
                const uint32_t seed = t->_surface ? IOSurfaceGetSeed(t->_surface) : 0;
                const uint64_t writes = t->_surface ? surfaceWrites(t->_surface) : 0;
                const raw::Texture *luma = rc->readTexture(t->_raw.luma);
                const uint64_t nativeGeneration = luma ? luma->nativeGeneration : 0;
                if (!t->_surface || !t->_imageCurrent || t->_imageSeed != seed || t->_imageWrites != writes ||
                    t->_imageNativeGeneration != nativeGeneration) {
                    stale.push_back(t);
                    converted.push_back({t, seed, writes, nativeGeneration});
#if SWITCH_FRAME_TIMELINE
                    traceSurfaceImage(t, "converted", seed);
#endif
                }
            }
            planar.swap(stale);
            // 4:4:4 code textures: two copy-engine remaps each
            bool copies = false;
            for (RMTexture *t : planar)
                copies |= t->_raw.planar;
            if (copies) {
                raw::BlitEncoder *conv = rc->blit();
                for (RMTexture *t : planar)
                    if (t->_raw.planar)
                        conv->convertPlanar(t->_raw);
                conv->end();
            }
            // YCbCr textures' planes whose rows or offset are 16 bytes apart but not 32 (ImageIO's photo thumbnails, a width of
            // 360: rows of 368) are sampled through pool images (raw::Texture::sampled, createSurfaceTexture): filled from the
            // planes by the copy engine first, as refresh() fills a sampled surface's (r7u: without it the conversion read
            // whatever the pool held, magenta and green stripes of older images)
            raw::BlitEncoder *fill = nullptr;
            for (RMTexture *t : planar)
                if (t->_raw.ycbcr)
                    for (const raw::Texture *x : {t->_raw.luma, t->_raw.chroma})
                        if (x && x->sampled && rc->readTexture(x) == x) {
                            if (!fill)
                                fill = rc->blit();
                            fill->copyTexels(*x, 0, 0, 0, *x->sampled, 0, 0, 0, x->width, x->height);
                        }
            if (fill)
                fill->end();
            // YCbCr textures: a pass of the pack's conversion state each, into the image the draws sample
            for (RMTexture *t : planar) {
                raw::Texture &x = t->_raw;
                if (!x.ycbcr)
                    continue;
                raw::PassDesc p;
                p.color[0].texture = &x;
                p.color[0].load = raw::ColorAttachment::DontCare;
                p.color[0].store = true;
                p.count = 1;
                raw::RenderEncoder *conv = rc->render(p);
                if (!conv)
                    return fail(PredEncPassRender, "a YCbCr texture's image the raw path cannot render");
                conv->setPipeline(d.ycbcrPipeline(x.ycbcr));
                conv->setBytes(2, 0, x.shape, sizeof x.shape);
                conv->setTexture(0, x.luma);
                conv->setTexture(1, x.chroma ? x.chroma : x.luma);  // (over plane 1: chroma codes 0, none read)
                conv->setViewport(0, 0, x.width, x.height, 0, 1);
                conv->setScissor(0, 0, x.width, x.height);
                if (!conv->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why))
                    return refused();
                conv->end();
            }
        }
    }
    RAW_ENCODE_END(resourceSpan);
    // textures sampled through a pool image (rows 16 but not 32 apart, raw::Texture::sampled): refreshed from their texels by the
    // copy engine before the first pass that samples them, and again after work here wrote them (m2v copies such a surface into
    // its shadow at each encoder that uses it once written since)
    std::set<RMTexture *> fresh;
    raw::Texture levelTargets[8];  // the open pass's level targets: the encoder reads them until its end()
#if SWITCH_RAW_DIAGNOSTICS
    uint64_t levelPasses = 0;
    static std::atomic<bool> loggedLevel{false};
#endif
    auto refresh = [&](size_t pass) {
        cb->_plan.forEachResource(pass, cb->_plan.encoderEnd(pass), RMResourceKind::Texture,
                                  [&](const RMRangeResource &use) {
            if (!(use.access & raw::ResourceUse::Sample))
                return;
            RMTexture *t = (__bridge RMTexture *)use.object;
            if (rc->readTexture(&t->_raw) != &t->_raw || !t->_raw.sampled ||
                (!t->_raw.surfaceView && !fresh.insert(t).second))
                return;
            const raw::Texture &x = t->_raw;
            raw::BlitEncoder *b = rc->blit();
            b->copyTexels(x, 0, 0, 0, *x.sampled, 0, 0, 0, x.width, x.height);
            b->end();
        });
    };
    // extended-range textures (raw_xr.h): a draw samples the stand-in, drawn from the codes by the pack's conversion state
    // before the first pass whose draws sample it, and again after work here writes the codes. Their passes' prologue barrier
    // orders the conversion between the codes' writes and the samples.
    std::set<RMTexture *> standInFresh;
    std::deque<raw::Texture>
        codes;  // the conversions' sources: the codes' headers (the encoder reads them until its end)
    auto standIns = [&](size_t pass) {
        std::vector<RMTexture *> stale;
        cb->_plan.forEachResource(pass, cb->_plan.encoderEnd(pass), RMResourceKind::Texture,
                                  [&](const RMRangeResource &use) {
            if (!(use.access & raw::ResourceUse::Sample))
                return;
            RMTexture *texture = (__bridge RMTexture *)use.object;
            if (texture->_raw.standIn && !standInFresh.count(texture))
                stale.push_back(texture);
        });
        for (RMTexture *t : stale) {
            raw::Texture &x = t->_raw;
            raw::PassDesc p;
            p.color[0].texture = x.standIn;
            p.color[0].load = raw::ColorAttachment::DontCare;
            p.color[0].store = true;
            p.count = 1;
            raw::RenderEncoder *conv = rc->render(p);
            if (!conv)
                return fail(PredEncPassRender, "an extended-range texture's stand-in the raw path cannot render");
            codes.push_back(x);
            codes.back().standIn = nullptr;
            useTexture(rc, t, false);
            conv->setPipeline(d.xrPipeline(raw::xrForm(x.format)));
            conv->setTexture(0, &codes.back());
            conv->setViewport(0, 0, x.width, x.height, 0, 1);
            conv->setScissor(0, 0, x.width, x.height);
            if (!conv->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why))
                return refused();
            conv->end();
            standInFresh.insert(t);
        }
        return true;
    };
    RAW_ENCODE_BEGIN(loopSpan, Loop);
    for (size_t at = from; at < to; at++) {
        const Cmd c = cb->_plan.legacy(at);
        if (c.op == OpRender || c.op == OpCompute || c.op == OpBlit)
            encoder = at;
        if (c.op == OpDispatch) {
            RAW_ENCODE_SPAN(Compute);
            const auto &dispatch = cb->_plan.dispatch(at);
            const auto *kernels = static_cast<const raw::ComputeKernelSet<raw::ComputeState> *>(dispatch.kernel);
            const auto &state = cb->_plan.kernelStates[dispatch.state];
            const auto &textures = cb->_plan.textures[2][state.textures].objects;
            std::array<RMTexture *, 31> shadows{};
            size_t count = 0;
            for (uint32_t slots = dispatch.surfaceSlots; slots; slots &= slots - 1) {
                RMTexture *texture = (__bridge RMTexture *)textures[__builtin_ctz(slots)];
                if (texture && rc->readTexture(&texture->_raw) == &texture->_raw &&
                    std::find(shadows.begin(), shadows.begin() + count, texture) == shadows.begin() + count)
                    shadows[count++] = texture;
            }
            auto copyShadows = [&](bool store) {
                ce->end();
                ce.reset();
                raw::BlitEncoder *copy = rc->blit();
                for (size_t i = 0; i < count; ++i) {
                    const raw::Texture &view = shadows[i]->_raw, &image = *view.sampled;
                    copy->copyTexels(store ? image : view, 0, 0, 0, store ? view : image, 0, 0, 0, view.width,
                                     view.height);
                }
                copy->end();
                ce.emplace(*rc, d);
                ce->setSerial(serialCompute);
                stateCursor.reset();
            };
            if (count)
                copyShadows(false);
            encodePlannedState(cb, at, *rc, re, &*ce, stateCursor);
            const auto &regions = dispatch.regions;
            if (!kernels || kernels->count != regions.count)
                return fail(PredCompute, "a dispatch with an incomplete region plan", c.op);
            for (uint8_t region = 0; region < regions.count; ++region) {
                ce->setState(kernels->jobs[region]->value());
                if (const char *w = nullptr;
                    !ce->dispatchRegion(regions.regions[region], regions.grid, regions.nominal, !region,
                                        region + 1 == regions.count, &w))
                    return fail(PredCompute, w, c.op);
            }
            if (count) {
                copyShadows(true);
                fresh.clear();
            }
            continue;
        }
        {
            RAW_ENCODE_SPAN(State);
            encodePlannedState(cb, at, *rc, re, ce ? &*ce : nullptr, stateCursor);
        }
        if (isKernelCommand(c.op)) {
            RAW_ENCODE_SPAN(Compute);
            if (const char *w = nullptr; !encodeKernel(*ce, *rc, cb, c, &w))
                return fail(PredCompute, w, c.op);
            continue;
        }
        switch (c.op) {
        case OpCompute:
            ce.emplace(*rc, d);
            serialCompute = c.a == MTLDispatchTypeSerial;
            ce->setSerial(serialCompute);
            break;
        case OpRender: {
            RAW_ENCODE_SPAN(Pass);
            const RMPass &pass = cb->_passes[c.x];
            renderBegin = at;
            if (continuing) {
                continuing = false;
                uint8_t stores = 0;
                for (uint32_t i = 0; i < 8; ++i)
                    if (pass.color[i].texture && pass.color[i].store == MTLStoreActionStore)
                        stores |= uint8_t(1u << i);
                re->continuePass(stores);
                planSplitPrograms(cb, at, *re);
                break;
            }
            if (cb->_sampledShadows)
                refresh(at);
            if (cb->_standIns && !standIns(at))
                return false;
            raw::PassDesc p;
            for (unsigned j = 0; j < 8; j++) {
                const RMPass::Color &a = pass.color[j];
                RMTexture *t = (RMTexture *)a.texture;
                if (!t)
                    continue;
                if (t->_raw.planar && !t->_raw.planarStandIn)
                    return fail(PredEncPlanarRT, "a planar texture as a render target", j);
                useTexture(rc, t, true);
                if (t->_raw.planarStandIn) {
                    // Metal's DontCare load keeps the planes' codes where no draw writes, and the store writes every texel
                    // (raw/test/standinoracle.mm): such a pass loads them too
                    const bool stores = a.store != MTLStoreActionDontCare;
                    if (a.load == MTLLoadActionLoad || (a.load == MTLLoadActionDontCare && stores))
                        standInLoads.push_back(t);
                    if (stores)
                        standInStores.push_back(t);
                }
                if (!fresh.empty())
                    fresh.erase(t);  // this pass writes it
                standInFresh.erase(t);
                if (t->_raw.chain && a.level) {
                    if (!raw::chainLevelTarget(t->_raw, uint32_t(a.level), levelTargets[j]))
                        return fail(PredEncPassRender, "a mip level the raw path cannot render to", a.level);
                    levelTargets[j].tic = gRM->raw->levelTic(t->_raw, uint32_t(a.level));
#if SWITCH_RAW_DIAGNOSTICS
                    levelPasses++;
                    if (!loggedLevel.exchange(true))
                        os_log(rawLog(), "raw: renders into mip levels (first: level %lu of %ux%u format %u)",
                               (unsigned long)a.level, t->_raw.width, t->_raw.height, t->_raw.format);
#endif
                }
                p.color[j].texture = a.level                 ? &levelTargets[j]
                                     : t->_raw.planarStandIn ? t->_raw.planarStandIn
                                                             : &t->_raw;
                p.color[j].load = a.load == MTLLoadActionClear  ? raw::ColorAttachment::Clear
                                  : a.load == MTLLoadActionLoad ? raw::ColorAttachment::Load
                                                                : raw::ColorAttachment::DontCare;
                p.color[j].store =
                    a.store == MTLStoreActionStore || a.store == MTLStoreActionStoreAndMultisampleResolve;
                if (a.store == MTLStoreActionMultisampleResolve || a.store == MTLStoreActionStoreAndMultisampleResolve)
                    if (RMTexture *r = (RMTexture *)a.resolve)
                        resolves.push_back({j, r});
                const MTLClearColor k = a.clear;
                p.color[j].clear[0] = float(k.red);
                p.color[j].clear[1] = float(k.green);
                p.color[j].clear[2] = float(k.blue);
                p.color[j].clear[3] = float(k.alpha);
                p.count = j + 1;
            }
            if (RMTexture *z = (RMTexture *)pass.depth) {  // (rawDepthStencil: decided at record)
                p.depthStencil.texture = &z->_raw;
                p.depthStencil.clearDepth = pass.depthLoad == MTLLoadActionClear;
                p.depthStencil.clearStencil = pass.stencilLoad == MTLLoadActionClear;
                p.depthStencil.depth = float(std::clamp(pass.clearDepth, 0.0, 1.0));
                p.depthStencil.stencil = pass.clearStencil;
            }
            p.width = uint32_t(std::min<NSUInteger>(pass.width, UINT32_MAX));
            p.height = uint32_t(std::min<NSUInteger>(pass.height, UINT32_MAX));
            p.storageMask = passStorageMask(cb, at);
            for (RMTexture *t : standInLoads)
                if (standInPass(t, false))
                    return false;
            standInLoads.clear();
            if (!(re = rc->render(p)))
                return fail(PredEncPassRender, "a pass the raw path cannot render to", p.count);
            planSplitPrograms(cb, at, *re);
            break;
        }
        case OpBlit:
            be = rc->blit();
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
            for (const auto &o : cb->_probeOps)
                if (o.at == at)
                    be->probe(o.op, o.read ? &o.read->_raw : nullptr, o.written->_raw);
#endif
            break;
        case OpEnd:
            if (re && at + 1 < to && cb->_plan.operation(at + 1).op == OpRender &&
                raw::canFusePasses(fusionPass(cb, renderBegin), fusionPass(cb, at + 1))) {
                continuing = true;
                break;
            }
            if (re)
                re->end();
            if (re && !resolvePasses())
                return false;
            if (be)
                be->end();
            if (ce) {
                ce->end();
                ce.reset();
            }
            for (RMTexture *t : standInStores)
                if (standInPass(t, true))
                    return false;
            standInStores.clear();
            re = nullptr;
            be = nullptr;
            break;
        case OpDraw:
            if (!re->draw(c.stage, c.a, c.b, uint32_t(c.c >> 32), uint32_t(c.c), why))
                return refused();
            break;
        case OpBarrier:
            re->barrier();
            break;
        case OpDrawIndexed: {
            const auto &draw = cb->_plan.draw(at);
            RMBuffer *indices = (__bridge RMBuffer *)draw.indices;
            useBuffer(rc, indices, false);
            if (!re->drawIndexed(draw.primitive, draw.count, draw.indexType == MTLIndexTypeUInt16 ? 2 : 4,
                                 &indices->_raw, draw.indexOffset, draw.instances, draw.baseVertex, draw.baseInstance,
                                 why))
                return refused();
            break;
        }
        case OpDrawIndirect:
        case OpDrawIndexedIndirect: {
            const auto &draw = cb->_plan.draw(at);
            RMBuffer *arguments = (__bridge RMBuffer *)draw.arguments;
            RMBuffer *indices = (__bridge RMBuffer *)draw.indices;
            useBuffer(rc, arguments, false);
            useBuffer(rc, indices, false);
            const bool accepted =
                c.op == OpDrawIndirect
                    ? re->drawIndirect(draw.primitive, &arguments->_raw, draw.argumentOffset, why)
                    : re->drawIndexedIndirect(draw.primitive, draw.indexType == MTLIndexTypeUInt16 ? 2 : 4,
                                              &indices->_raw, draw.indexOffset, &arguments->_raw, draw.argumentOffset,
                                              why);
            if (!accepted)
                return refused();
            break;
        }
        case OpMipmaps: {
            RAW_ENCODE_SPAN(Blit);
            if (!((__bridge RMTexture *)c.obj)->_raw.chain->fmt2d) {  // the 3D engine's passes (decided at record)
                be->end();
                if (!mipPasses(rc, (__bridge RMTexture *)c.obj, why))
                    return refused();
                be = rc->blit();
                break;
            }
            encodeBlit(cb, c, rc, be, fresh, standInFresh);
            break;
        }
        case OpCopyTexture:
        case OpCopyBuffer:
        case OpFillBuffer:
        case OpCopyBufferToTexture:
        case OpCopyTextureToBuffer: {
            RAW_ENCODE_SPAN(Blit);
            encodeBlit(cb, c, rc, be, fresh, standInFresh);
            break;
        }
        default:  // states, fences, events, resources the raw path needs nothing for
            break;
        }
    }
    RAW_ENCODE_END(loopSpan);
    RAW_ENCODE_SPAN(Tail);
    if (re)
        re->end();
    if (re && !resolvePasses())
        return false;
    if (be)
        be->end();
    if (ce)
        ce->end();
    for (RMTexture *t : standInStores)
        if (standInPass(t, true))
            return false;
    encoder = to;
    uint32_t seq = 0;
    prepareSharedEvents(cb, rc);
    if (!rc->commit(seq)) {
        cb->_partial = rc->partial;
        if (rc->admissionBlocked) {
            RMQueue *queue = cb->_queue;
            int armed = KERN_SUCCESS;
            cb->_resourceReady = rc->resourceReadiness([queue] { wakeSubmit((__bridge void *)queue); }, armed);
            if (!cb->_resourceReady) {
                std::lock_guard<std::mutex> lock(cb->_lock);
                cb->_segError = [NSError errorWithDomain:MTLCommandBufferErrorDomain
                                                   code:MTLCommandBufferErrorInternal
                                               userInfo:@{NSLocalizedDescriptionKey : @"Resource readiness notification could not be armed"}];
            }
            rc->abandon();
            return false;
        }
        stale = rc->nativeError == kIOReturnNotReady && !rc->partial;
        if (stale)
            rc->invalidateNativeReads();
        else if (rc->nativeError && !rc->partial)
            failCommandBuffer(cb, @"Native ownership or event dependency failed");
        if (stale) {
            rc->abandon();
            return false;
        }
        return fail(PredEncKernel, "the kernel refused the submission");
    }
    publishSharedEvents(cb);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    cb->_probeTaken.add(rc->probeTaken());
#endif
    for (const Conversion &c : converted) {  // (submitted: their images hold the surfaces at these seeds)
        c.t->_imageSeed = c.seed;
        c.t->_imageWrites = c.writes;
        c.t->_imageNativeGeneration = c.nativeGeneration;
        c.t->_imageCurrent = true;
    }
    noteSurfaceWrites(cb, from, to);
    for (RMTexture *t : planesWritten)  // a write the surface's seed does not show: its image is converted again
        t->_imageCurrent = false;
    cb->_seqno = seq;
    cb->_channelGeneration = generation;
    cb->_stamp = cb->_segs.size() == 1 && cb->_segs.front().raw ? rc->stamp : 0;
    RAW_DIAGNOSTIC(d.stats.levelPasses += levelPasses);
    RAW_DIAGNOSTIC(gRM->stats.draws += rc->draws);
    RAW_DIAGNOSTIC(gRM->stats.passes += rc->passes);
    RAW_DIAGNOSTIC(gRM->stats.copies += rc->blits);
    return true;
}

bool encodeRaw(RMCommandBuffer *cb, size_t from, size_t to, const char **why, size_t *refusedEncoder)
{
    if (RMTexture *feedback = (__bridge RMTexture *)cb->_plan.nativeRangeRefusal(from, to)) {
        *why = "a planar texture sampled and rendered in one command buffer";
        hit(cb, PredEncPlanarRT, feedback->_raw.width, feedback->_raw.height);
        return false;
    }
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        bool stale = false;
        if (encodeRawAttempt(cb, from, to, why, stale, refusedEncoder))
            return true;
        if (!stale)
            return false;
    }
    *why = "the native surface changed repeatedly before submission";
    failCommandBuffer(cb, @"Native surface changed during submission");
    return false;
}
