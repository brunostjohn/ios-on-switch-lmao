// SPDX-License-Identifier: MIT
// The facade's half of private textures on compressed memory (raw_private_comp.h): which descriptors qualify, and the
// move to plain memory before anything names a texture's pages another way.
#import "RawMetalInternal.h"
#include "raw_storage_form.h"
#include "raw_view.h"
#include "raw_xr.h"

thread_local bool tRawEncoding = false;

std::recursive_mutex &submitLock()
{
    static std::recursive_mutex lock;
    return lock;
}

uint32_t privateCompressedKind(MTLTextureDescriptor *d)
{
    const raw::FormatInfo *f = raw::formatInfo(uint32_t(d.pixelFormat));
    raw::PrivateTextureFacts facts;
    facts.privateStorage = d.storageMode == MTLStorageModePrivate;
    facts.oneLevel = d.mipmapLevelCount == 1;
    facts.oneSample = d.sampleCount <= 1;
    facts.twoD = d.textureType == MTLTextureType2D;
    facts.renderedAndSampledOnly = d.usage != MTLTextureUsageUnknown &&
                                   !(d.usage & ~NSUInteger(MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead));
    facts.plainColour =
        f && f->rt && !raw::blockCompressed(f) && raw::xrForm(uint32_t(d.pixelFormat)) == raw::XrForm::None;
    facts.bpp = f ? f->bpp : 0;
    facts.compression = (gRM->raw->kernelInfo().flags & TegraGM20BInfoCompression) != 0;
    return raw::privateCompressedKind(facts);
}

bool rmTexturePlain(RMTexture *t)
{
    if (!t || !t->_rawOk || !gRM->raw->privateCompressed(t->_raw))
        return true;
    std::lock_guard<std::recursive_mutex> l(submitLock());
    if (tRawEncoding) {
        SWITCH_REFUSE_DETAIL("raw", "comp.private", t->_descriptor.pixelFormat,
                             "a compressed %ux%u texture of format %lu was asked for its pages inside a raw encode",
                             t->_raw.width, t->_raw.height, (unsigned long)t->_descriptor.pixelFormat);
        return false;
    }
    @synchronized(t) {
        if (!gRM->raw->privateCompressed(t->_raw))
            return true;
        if (!gRM->raw->plainFromCompressed(t->_raw))
            return false;
    }
    gRM->rawSinceM2v = true;
    return true;
}

void rmRangePlain(RMCommandBuffer *cb, size_t from, size_t to)
{
    cb->_plan.forEachResource(from, to, RMResourceKind::Texture, [](const RMRangeResource &use) {
        RMTexture *t = (__bridge RMTexture *)use.object;
        while (t && t->_kind == RMTextureView)
            t = t->_parent;
        rmTexturePlain(t);
    });
}

static raw::Pipeline *drawnPipeline(const raw::RasterState &raster)
{
    if (auto *variant = static_cast<raw::Variant<raw::Pipeline> *>(raster.nativeVariant))
        return variant->value();
    if (raster.nativePipeline)
        return static_cast<raw::Pipeline *>(raster.nativePipeline);
    id pipeline = (__bridge id)raster.pipeline;
    return [pipeline isKindOfClass:[RMPipeline class]] ? ((RMPipeline *)pipeline)->_raw : nullptr;
}

// What the draw's storage view would answer of the attachment once it is plain: only a compressed one that would fit
// then is worth the move
static bool fitsOncePlain(const raw::Pipeline &pl, uint32_t slot, const raw::Texture &t)
{
    raw::Texture plain = t;
    plain.comptags = false;
    return raw::storageViewMismatch(raw::texelBytes(pl.storageTic[slot]), pl.rec->storageSamples, plain) ==
           raw::StorageFallback::None;
}

void rmStorageTargetsPlain(RMCommandBuffer *cb, size_t from, size_t to)
{
    const RMPass *pass = nullptr;
    for (size_t at = from; at < to; ++at) {
        const Cmd c = cb->_plan.legacy(at);
        if (c.op == OpRender)
            pass = &cb->_passes[c.x];
        else if (c.op == OpCompute || c.op == OpBlit)
            pass = nullptr;
        if (!pass || !isPlanDraw(c.op))
            continue;
        const raw::RenderStateRef &state = cb->_plan.renderStates[cb->_plan.draw(at).state];
        const raw::Pipeline *pl = drawnPipeline(cb->_plan.raster[state.raster]);
        if (!pl || !pl->rec)
            continue;
        const uint8_t slots = raw::storageSlotsToPlain(pl->storageBits, pl->barrierTwin != nullptr);
        for (uint32_t slot = 0; slot < 8; ++slot)
            if (RMTexture *t = (RMTexture *)pass->color[slot].texture;
                (slots >> slot & 1) && t && t->_raw.comptags && fitsOncePlain(*pl, slot, t->_raw))
                rmTexturePlain(t);
    }
}
