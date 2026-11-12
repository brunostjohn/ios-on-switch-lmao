// SPDX-License-Identifier: MIT
#import "RawMetalResolveAsk.h"

raw::ResolveTarget resolveTarget(RMTexture *into)
{
    raw::ResolveTarget t;
    for (RMTexture *x = into; x; x = x->_parent) {
        MTLTextureDescriptor *d = x->_descriptor;
        t.sharedStorage |= d.storageMode == MTLStorageModeShared ||
                           (x->_heap && x->_heap->_descriptor.storageMode == MTLStorageModeShared);
        t.imported |= x->_kind == RMTextureSurface || x->_surface || x->_raw.import;
        t.overBuffer |= x->_kind == RMTextureBuffer || x->_buffer;
        t.storage |= (d.usage & MTLTextureUsageShaderWrite) != 0;
        t.readOut |= x->_readOut.load(std::memory_order_relaxed);
    }
    return t;
}

raw::ResolveAsk resolveAsk(RMTexture *into, const raw::Texture &rendered, uint32_t width, uint32_t height)
{
    raw::ResolveAsk a;
    a.format = uint16_t(into->_raw.format);
    a.samples = uint8_t(rendered.samples);
    a.readable = raw::resolveTargetReadable(resolveTarget(into));
    a.wholeTarget = width == into->_raw.width && height == into->_raw.height;
    a.shadowTarget = into->_raw.shadowTarget;
    a.compressedSamples = rendered.comptags;
    return a;
}
