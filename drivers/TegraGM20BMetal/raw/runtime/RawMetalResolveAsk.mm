// SPDX-License-Identifier: MIT
#import "RawMetalResolveAsk.h"

raw::ResolveTarget resolveTarget(RMTexture *into)
{
    raw::ResolveTarget t;
    for (RMTexture *x = into; x; x = x->_parent) {
        MTLTextureDescriptor *d = x->_descriptor;
        t.sharedStorage |= d.storageMode == MTLStorageModeShared ||
                           (x->_heap && x->_heap->_descriptor.storageMode == MTLStorageModeShared);
        const bool surface = x->_kind == RMTextureSurface || x->_surface;
        const bool renderTargetOnly = (d.usage & ~MTLTextureUsageRenderTarget) == 0;
        t.imported |= surface ? !renderTargetOnly : x->_raw.import != nullptr;
        t.overBuffer |= x->_kind == RMTextureBuffer || x->_buffer;
        t.storage |= (d.usage & MTLTextureUsageShaderWrite) != 0;
        t.readOut |= x->_readOut.load(std::memory_order_relaxed);
    }
    return t;
}

#if SWITCH_RAW_DIAGNOSTICS
// Once a reason: what kept a screen-format resolve off the 2D engine, so the device log names the fact
static void sayReadable(RMTexture *into, const raw::ResolveTarget &t, uint32_t samples)
{
    static std::atomic<uint32_t> said{0};
    const uint32_t why = t.sharedStorage | t.imported << 1 | t.overBuffer << 2 | t.storage << 3 | t.readOut << 4;
    if (said.fetch_or(why) & why)
        return;
    os_log(rawLog(),
           "raw: resolve.twod refused a %ux %ux%u format %u resolve as readable: shared=%d imported=%d buffer=%d "
           "storage=%d readout=%d usage=0x%lx kind=%u",
           samples, into->_raw.width, into->_raw.height, into->_raw.format, t.sharedStorage, t.imported, t.overBuffer,
           t.storage, t.readOut, (unsigned long)into->_descriptor.usage, unsigned(into->_kind));
}
#endif

raw::ResolveAsk resolveAsk(RMTexture *into, const raw::Texture &rendered, uint32_t width, uint32_t height)
{
    raw::ResolveAsk a;
    a.format = uint16_t(into->_raw.format);
    a.samples = uint8_t(rendered.samples);
    const raw::ResolveTarget target = resolveTarget(into);
    a.readable = raw::resolveTargetReadable(target);
#if SWITCH_RAW_DIAGNOSTICS
    if (a.readable && (a.format == 70 || a.format == 80) && (a.samples == 2 || a.samples == 4))
        sayReadable(into, target, a.samples);
#endif
    a.wholeTarget = width == into->_raw.width && height == into->_raw.height;
    a.shadowTarget = into->_raw.shadowTarget;
    a.compressedSamples = rendered.comptags;
    return a;
}
