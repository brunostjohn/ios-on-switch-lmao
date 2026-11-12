// SPDX-License-Identifier: MIT
#import "RawMetalXrPlanes.h"

bool xrPlanesShape(MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane)
{
    static const SEL rotation = NSSelectorFromString(@"rotation");
    const NSUInteger turned =
        [d respondsToSelector:rotation] ? ((NSUInteger (*)(id, SEL))objc_msgSend)(d, rotation) : 0;
    return raw::xrForm(uint32_t(d.pixelFormat)) == raw::XrForm::Planes && plane == 0 &&
           IOSurfaceGetPlaneCount(s) == 2 && d.textureType == MTLTextureType2D && d.mipmapLevelCount == 1 &&
           d.sampleCount == 1 && !turned && d.usage &&
           !(d.usage & ~(MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget)) &&
           IOSurfaceGetWidthOfPlane(s, 0) == d.width && IOSurfaceGetHeightOfPlane(s, 0) == d.height &&
           IOSurfaceGetWidthOfPlane(s, 1) == d.width && IOSurfaceGetHeightOfPlane(s, 1) == d.height &&
           IOSurfaceGetBytesPerElementOfPlane(s, 0) == 4 && IOSurfaceGetBytesPerElementOfPlane(s, 1) == 1;
}

bool makeXrPlanes(RMTexture *t, IOSurfaceRef s)
{
    void *base = IOSurfaceGetBaseAddress(s);
    const uintptr_t at = uintptr_t(base);
    MTLTextureDescriptor *d = t->_descriptor;
    const uint64_t word = uintptr_t(IOSurfaceGetBaseAddressOfPlane(s, 0)) - at,
                   alpha = uintptr_t(IOSurfaceGetBaseAddressOfPlane(s, 1)) - at;
    const uint32_t wordPitch = uint32_t(IOSurfaceGetBytesPerRowOfPlane(s, 0)),
                   alphaPitch = uint32_t(IOSurfaceGetBytesPerRowOfPlane(s, 1));
    if (!gRM->raw->createXrPlanesTexture(IOSurfaceGetID(s), base, IOSurfaceGetAllocSize(s), s, word, wordPitch, alpha,
                                         alphaPitch, uint32_t(d.width), uint32_t(d.height), t->_raw)) {
        SWITCH_REFUSE_DETAIL(
            "raw", "xr.planes", 0,
            "no raw texture over a %lux%lu two-plane surface (planes at %llu and %llu, rows %u and %u, "
            "base&0xfff 0x%lx): m2v's",
            (unsigned long)d.width, (unsigned long)d.height, (unsigned long long)word, (unsigned long long)alpha,
            wordPitch, alphaPitch, (unsigned long)(at & 0xfff));
        return false;
    }
    keepExtendedRange(d, t->_raw, false);
    surfaceConverted(t->_surfaceWrites, true);
    static std::atomic<bool> said{false};
    if (!said.exchange(true))
        os_log(rawLog(), "raw: two-plane XR on (%ux%u '%c%c%c%c', planes merged and split on the copy engine)",
               unsigned(d.width), unsigned(d.height), char(IOSurfaceGetPixelFormat(s) >> 24),
               char(IOSurfaceGetPixelFormat(s) >> 16), char(IOSurfaceGetPixelFormat(s) >> 8),
               char(IOSurfaceGetPixelFormat(s)));
    return true;
}

__attribute__((objc_externally_retained)) XrPlaneSync::Entry *XrPlaneSync::entry(RMTexture *t)
{
    if (!t || !xrOverPlanes(t->_raw))
        return nullptr;
    for (Entry &e : entries_)
        if (e.t == t)
            return &e;
    const uint32_t seed = IOSurfaceGetSeed(t->_surface);
    const uint64_t writes = surfaceWrites(t->_surfaceWrites);
    const bool fresh = t->_imageCurrent && t->_imageSeed == seed && t->_imageWrites == writes;
    return &entries_.emplace_back(Entry{t, seed, writes, fresh, false});
}

void XrPlaneSync::merge(Entry &e, raw::BlitEncoder *open)
{
    if (e.fresh)
        return;
    e.fresh = true;
    if (open) {
        open->xrPlanes(e.t->_raw, false);
        open->barrier();
        return;
    }
    raw::BlitEncoder *b = rc_.blit();
    b->xrPlanes(e.t->_raw, false);
    b->end();
}

__attribute__((objc_externally_retained)) void XrPlaneSync::read(RMTexture *t, raw::BlitEncoder *open)
{
    if (Entry *e = entry(t))
        merge(*e, open);
}

// A Clear load that reaches every texel of `t`: in a pass with a draw call Metal's Clear reaches only the render area,
// rounded up to 32-pixel tiles (RenderEncoder::clears), so the render area (the smallest attachment, or the pass's own
// size) covers the texture
static bool clearsWhole(RMTexture *t, MTLLoadAction load, const RMPass &pass)
{
    if (load != MTLLoadActionClear)
        return false;
    NSUInteger width = pass.width ?: NSUIntegerMax, height = pass.height ?: NSUIntegerMax;
    for (const RMPass::Color &c : pass.color)
        if (id<MTLTexture> x = c.texture) {
            width = std::min(width, x.width);
            height = std::min(height, x.height);
        }
    if (id<MTLTexture> z = pass.depth) {
        width = std::min(width, z.width);
        height = std::min(height, z.height);
    }
    return width >= t->_raw.width && height >= t->_raw.height;
}

__attribute__((objc_externally_retained)) void XrPlaneSync::attach(RMTexture *t, MTLLoadAction load, const RMPass &pass)
{
    Entry *e = entry(t);
    if (!e)
        return;
    if (clearsWhole(t, load, pass))
        e->fresh = true;
    merge(*e, nullptr);
    e->written = true;
}

void XrPlaneSync::blit(const Cmd &c, raw::BlitEncoder *be)
{
    if (Entry *e = entry(c.op == OpCopyTextureToBuffer ? (__bridge RMTexture *)c.obj : nil))
        merge(*e, be);
    if (Entry *e = entry(c.op == OpCopyBufferToTexture ? (__bridge RMTexture *)c.obj2 : nil)) {
        merge(*e, be);
        e->written = true;
    }
}

void XrPlaneSync::flush()
{
    raw::BlitEncoder *b = nullptr;
    for (const Entry &e : entries_)
        if (e.written) {
            if (!b)
                b = rc_.blit();
            b->xrPlanes(e.t->_raw, true);
        }
    if (b)
        b->end();
}

void XrPlaneSync::submitted()
{
    for (const Entry &e : entries_) {
        e.t->_imageSeed = e.seed;
        e.t->_imageWrites = e.written ? surfaceWrites(e.t->_surfaceWrites) : e.writes;
        e.t->_imageCurrent = true;
    }
}
