// SPDX-License-Identifier: MIT
#import "RawMetalRangeEncode.h"
#import "RawMetalPlanAdapters.h"
#import "RawMetalPassFusion.h"
#import "RawMetalReach.h"
#import "RawMetalSplitPlan.h"
#import "RawMetalIndirect.h"
#import "RawMetalTargetViews.h"
#include "raw_compute_lookup.h"
#include "raw_clear.h"

// textures sampled through a pool image (rows 16 but not 32 apart, raw::Texture::sampled): refreshed from their texels by the
// copy engine before the first pass that samples them, and again after work here wrote them (m2v copies such a surface into
// its shadow at each encoder that uses it once written since)
void RMRangeEncode::refresh(size_t pass)
{
    cb_->_plan.forEachResource(pass, cb_->_plan.encoderEnd(pass), RMResourceKind::Texture,
                               [&](const RMRangeResource &use) {
                                   if (!(use.access & raw::ResourceUse::Sample))
                                       return;
                                   __unsafe_unretained RMTexture *t = (__bridge RMTexture *)use.object;
                                   if (rc_->readTexture(&t->_raw) != &t->_raw || !t->_raw.sampled ||
                                       (!t->_raw.surfaceView && !fresh_.insert(t).second))
                                       return;
                                   const raw::Texture &x = t->_raw;
                                   raw::BlitEncoder *b = rc_->blit();
                                   b->copyTexels(x, 0, 0, 0, *x.sampled, 0, 0, 0, x.width, x.height);
                                   b->end();
                               });
}

// extended-range textures (raw_xr.h): a draw samples the stand-in, drawn from the codes by the pack's conversion state
// before the first pass whose draws sample it, and again after work here writes the codes. Their passes' prologue barrier
// orders the conversion between the codes' writes and the samples.
bool RMRangeEncode::standIns(size_t pass)
{
    std::vector<__unsafe_unretained RMTexture *> stale;
    cb_->_plan.forEachResource(pass, cb_->_plan.encoderEnd(pass), RMResourceKind::Texture,
                               [&](const RMRangeResource &use) {
                                   if (!(use.access & raw::ResourceUse::Sample))
                                       return;
                                   __unsafe_unretained RMTexture *texture = (__bridge RMTexture *)use.object;
                                   if (texture->_raw.standIn && !standInFresh_.count(texture) &&
                                       (!texture->_raw.xrPlanes || cb_->_xrStandIns.count(texture)))
                                       stale.push_back(texture);
                               });
    for (__unsafe_unretained RMTexture *t : stale) {
        raw::Texture &x = t->_raw;
        raw::PassDesc p;
        p.color[0].texture = x.standIn;
        p.color[0].load = raw::ColorAttachment::DontCare;
        p.color[0].store = true;
        p.count = 1;
        raw::RenderEncoder *conv = rc_->render(p);
        if (!conv)
            return fail(PredEncPassRender, "an extended-range texture's stand-in the raw path cannot render");
        codes_.push_back(x);
        codes_.back().standIn = nullptr;
        useTexture(rc_, t, false);
        conv->setPipeline(d_->xrPipeline(raw::xrForm(x.format), x.xrPlanes));
        conv->setTexture(0, x.xrPlanes ? x.luma : &codes_.back());
        if (x.xrPlanes)
            conv->setTexture(1, x.chroma);
        conv->setViewport(0, 0, x.width, x.height, 0, 1);
        conv->setScissor(0, 0, x.width, x.height);
        if (!conv->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why_))
            return refused();
        conv->end();
        standInFresh_.insert(t);
    }
    return true;
}

bool RMRangeEncode::command(size_t at)
{
    RMCommandBuffer *cb = cb_;
    raw::CommandBuffer *rc = rc_;
    const Cmd c = cb->_plan.legacy(at);
    if ((c.op == OpRender && !rmContinuesPass(cb, at)) || c.op == OpCompute || c.op == OpBlit) {
        encoder_ = at;
#if SWITCH_RAW_DIAGNOSTICS
        traceFreshUses(cb, at);
#endif
    }
    if (c.op == OpDispatch) {
        RAW_ENCODE_SPAN(Compute);
        const auto &dispatch = cb->_plan.dispatch(at);
        const auto *kernels = static_cast<const raw::ComputeKernelSet<raw::ComputeState> *>(dispatch.kernel);
        const auto &state = cb->_plan.kernelStates[dispatch.state];
        const auto &textures = cb->_plan.textures[2][state.textures].objects;
        std::array<__unsafe_unretained RMTexture *, 31> shadows{};
        size_t count = 0;
        for (uint32_t slots = dispatch.surfaceSlots; slots; slots &= slots - 1) {
            __unsafe_unretained RMTexture *texture = (__bridge RMTexture *)textures[__builtin_ctz(slots)];
            if (texture && rc->readTexture(&texture->_raw) == &texture->_raw &&
                std::find(shadows.begin(), shadows.begin() + count, texture) == shadows.begin() + count)
                shadows[count++] = texture;
        }
        auto copyShadows = [&](bool store) {
            ce_->end();
            ce_.reset();
            raw::BlitEncoder *copy = rc->blit();
            for (size_t i = 0; i < count; ++i) {
                const raw::Texture &view = shadows[i]->_raw, &image = *view.sampled;
                copy->copyTexels(store ? image : view, 0, 0, 0, store ? view : image, 0, 0, 0, view.width, view.height);
            }
            copy->end();
            ce_.emplace(*rc, *d_);
            ce_->setSerial(serialCompute_);
            stateCursor_.reset();
        };
        if (count)
            copyShadows(false);
        encodePlannedState(cb, at, *rc, re_, &*ce_, stateCursor_);
        const auto &regions = dispatch.regions;
        ce_->setStageInRegion(dispatch.stageIn);
        if (!kernels || kernels->count != regions.count)
            return fail(PredCompute, "a dispatch with an incomplete region plan", c.op);
        for (uint8_t region = 0; region < regions.count; ++region) {
            ce_->setState(kernels->jobs[region]->value());
            if (const char *w = nullptr; !ce_->dispatchRegion(regions.regions[region], regions.grid, regions.nominal,
                                                              !region, region + 1 == regions.count, &w))
                return fail(PredCompute, w, c.op);
        }
        if (count) {
            copyShadows(true);
            fresh_.clear();
        }
        return true;
    }
    {
        RAW_ENCODE_SPAN(State);
        encodePlannedState(cb, at, *rc, re_, ce_ ? &*ce_ : nullptr, stateCursor_);
    }
    if (isKernelCommand(c.op)) {
        RAW_ENCODE_SPAN(Compute);
        if (const char *w = nullptr; !encodeKernel(*ce_, *rc, cb, c, at, &w))
            return fail(PredCompute, w, c.op);
        return true;
    }
    switch (c.op) {
    case OpCompute:
        if (!computeUnit_ || serialCompute_ != (c.a == MTLDispatchTypeSerial))
            rc->endPredicationUnit();
        computeUnit_ = true;
        ce_.emplace(*rc, *d_);
        serialCompute_ = c.a == MTLDispatchTypeSerial;
        ce_->setSerial(serialCompute_);
        counters_->started(at, *ce_);
        break;
    case OpRender:
        return beginPass(at);
    case OpBlit:
        computeUnit_ = false;
        rc->endPredicationUnit();
        be_ = rc->blit();
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
        for (const auto &o : cb->_probeOps)
            if (o.at == at)
                be_->probe(o.op, o.read ? &o.read->_raw : nullptr, o.written->_raw);
#endif
        counters_->started(at, *be_);
        break;
    case OpEnd:
        return endEncoder(at);
    case OpDraw:
        if (!re_->draw(c.stage, c.a, c.b, uint32_t(c.c >> 32), uint32_t(c.c), why_))
            return refused();
        break;
    case OpBarrier:
        re_->barrier();
        break;
    case OpVisibility:
        if (!re_->setVisibility(
                c.a,
                [(__bridge id)c.obj isKindOfClass:[RMBuffer class]] ? &((__bridge RMBuffer *)c.obj)->_raw : nullptr,
                c.c))
            return fail(PredCallVisibility, "a visibility offset the raw path cannot take", c.a, c.c);
        break;
    case OpDrawIndexed: {
        const auto &draw = cb->_plan.draw(at);
        __unsafe_unretained RMBuffer *indices = (__bridge RMBuffer *)draw.indices;
        useBuffer(rc, indices, false);
        if (!re_->drawIndexed(draw.primitive, draw.count, draw.indexType == MTLIndexTypeUInt16 ? 2 : 4, &indices->_raw,
                              draw.indexOffset, draw.instances, draw.baseVertex, draw.baseInstance, why_))
            return refused();
        break;
    }
    case OpDrawIndirect:
    case OpDrawIndexedIndirect: {
        const auto &draw = cb->_plan.draw(at);
        __unsafe_unretained RMBuffer *arguments = (__bridge RMBuffer *)draw.arguments;
        __unsafe_unretained RMBuffer *indices = (__bridge RMBuffer *)draw.indices;
        useBuffer(rc, arguments, false);
        useBuffer(rc, indices, false);
        const bool accepted =
            c.op == OpDrawIndirect
                ? re_->drawIndirect(draw.primitive, &arguments->_raw, draw.argumentOffset, why_)
                : re_->drawIndexedIndirect(draw.primitive, draw.indexType == MTLIndexTypeUInt16 ? 2 : 4, &indices->_raw,
                                           draw.indexOffset, &arguments->_raw, draw.argumentOffset, why_);
        if (!accepted)
            return refused();
        break;
    }
    case OpExecuteCommands:
        if (!encodeCommands(re_, rc, cb->_plan.draw(at), why_))
            return refused();
        break;
    case OpDrawPatches:
    case OpDrawPatchesIndirect:
        if (!encodePatches(re_, rc, cb->_plan.draw(at), cb->_plan.patches(at), c.op == OpDrawPatchesIndirect, why_))
            return refused();
        break;
    case OpResetCommands:
    case OpCopyCommands:
        encodeCommandsBlit(be_, c);
        break;
    case OpMipmaps: {
        RAW_ENCODE_SPAN(Blit);
        if (!encodeMipmaps(rc, (__bridge RMTexture *)c.obj, uint32_t(c.c), be_, why_))
            return refused();
        break;
    }
    case OpFillTexture: {
        RAW_ENCODE_SPAN(Blit);
        be_->end();
        if (!fillTexturePass(cb, c, rc))
            return fail(PredBlitTexFill, "a texture fill into a target the raw path cannot render",
                        ((__bridge RMTexture *)c.obj)->_descriptor.pixelFormat);
        be_ = rc->blit();
        break;
    }
    case OpCopyTexture:
    case OpCopyBuffer:
    case OpFillBuffer:
    case OpCopyBufferToTexture:
    case OpCopyTextureToBuffer: {
        RAW_ENCODE_SPAN(Blit);
        encodeBlit(cb, c, rc, be_, fresh_, standInFresh_);
        break;
    }
    case OpSignalEvent:
        if (!isInlineSignal(cb, at))
            break;
        computeUnit_ = false;
        rc->endPredicationUnit();
        if (!releaseInlineSignal(cb, at, rc)) {
            encoder_ = SIZE_MAX;
            return fail(PredEvent, "a signal inside a raw range whose event the kernel no longer holds");
        }
        break;
    default:  // states, fences, events, resources the raw path needs nothing for
        break;
    }
    return true;
}
