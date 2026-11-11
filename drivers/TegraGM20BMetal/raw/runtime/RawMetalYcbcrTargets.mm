// SPDX-License-Identifier: MIT
// Two-plane YCbCr code textures as render targets (raw_ycbcr_store.h): Metal holds a pass's texels as halves and stores each
// texel's luma code and each chroma block's rounded mean code. The raw path renders the pass into the texture's RGBA32Float
// stand-in (its pipelines are made for that format in the attachment's place), the pack's two-plane store state then writes
// the luma plane and each texel's chroma codes (RG8Unorm), and the 2D engine halves those into the chroma blocks: its 2:1
// bilinear step weighs the texels it halves by one half each and rounds half up, which is Metal's mean (fx::referenceDownsample).
// The ROP cannot move green into a one-channel target or blue and red into a pair, so the store is a draw; the mean is the 2D
// engine's. A pass that loads the texture is refused: Metal reloads the chroma interpolated between blocks.
#import "RawMetalInternal.h"
#include "raw_ycbcr_store.h"

void makeYcbcrTarget(raw::Texture &t, uint32_t pixelFormat)
{
    constexpr uint32_t kRG8Unorm = 30;
    raw::Device &d = *gRM->raw;
    raw::YcbcrCodeTarget form;
    bool made = raw::ycbcrCodeTarget(pixelFormat, form) && !raw::ycbcrTargetRefusal(pixelFormat) && t.luma &&
                t.chroma && d.standInPipeline(true) && (t.luma->renderable || t.luma->shadowTarget);
    raw::Texture *standIn = new raw::Texture, *codes = new raw::Texture, *halved = new raw::Texture;
    made = made && d.createTexture(t.width, t.height, raw::kYcbcrTargetStandIn, *standIn, false, true) &&
           standIn->renderable && d.createTexture(t.width, t.height, kRG8Unorm, *codes) && codes->renderable &&
           !codes->blockLinear && d.createTexture(t.chroma->width, t.chroma->height, kRG8Unorm, *halved, false, true) &&
           halved->blockLinear;
    if (!made) {
        for (raw::Texture *x : {standIn, codes, halved}) {
            d.destroyTexture(*x);
            delete x;
        }
        SWITCH_REFUSE_DETAIL("raw", "ycbcr.target", pixelFormat,
                             "no stand-in for a %ux%u format %u render target: its passes go to m2v (enc.planarrt)",
                             t.width, t.height, pixelFormat);
        return;
    }
    t.planarStandIn = standIn;
    t.chromaCodes = codes;
    t.chromaHalved = halved;
}

const char *ycbcrTargetPassRefusal(const raw::Texture &t, MTLRenderPassColorAttachmentDescriptor *a)
{
    const char *why = !t.planarStandIn || !t.chromaCodes ? "a YCbCr texture with no stand-in as a render target"
                      : a.loadAction == MTLLoadActionLoad
                          ? "a YCbCr code target loaded: Metal reloads its chroma interpolated between blocks"
                          : nullptr;
    if (why)
        SWITCH_REFUSE_DETAIL("raw", "pass.ycbcr", a.loadAction, "%ux%u format %u: %s", t.width, t.height,
                             uint32_t(a.texture.pixelFormat), why);
    return why;
}

void storeYcbcrChroma(raw::CommandBuffer *rc, const raw::Texture &t)
{
    raw::BlitEncoder *b = rc->blit();
    b->halveChroma(*t.chromaCodes, *t.chromaHalved);
    b->copyTexels(*t.chromaHalved, 0, 0, 0, *t.chroma, 0, 0, 0, t.chroma->width, t.chroma->height);
    b->end();
    static std::atomic<bool> said{false};
    if (!said.exchange(true, std::memory_order_relaxed))
        os_log(rawLog(),
               "raw: YCbCr code target stored on the GPU (first: %ux%u, chroma %ux%u): stand-in pass, two-plane store, "
               "chroma halved on the 2D engine",
               t.width, t.height, t.chroma->width, t.chroma->height);
}

uint32_t ycbcrTargetFormat(MTLRenderPipelineDescriptor *d)
{
    for (NSUInteger c = 0; c < 8; c++)
        if (raw::ycbcrCodeFormat(uint32_t(d.colorAttachments[c].pixelFormat)))
            return uint32_t(d.colorAttachments[c].pixelFormat);
    return 0;
}

@implementation RMDevice (YcbcrTargets)

// Decided before any compile: the raw path renders no 10-bit code target, nor a blended or channel-masked one (Metal's store
// of those is not measured): such a state is m2v's alone, as it was, without the raw path's translation (3-13 s in
// cameracaptured, then refused). An 8-bit one's state is made for its stand-in format. The m2v twin is of that format too:
// m2v renders no pass into a code target, so it only answers for the state. The archives a creation under
// FailOnBinaryArchiveMiss lists answer for the app's state, not its stand-in.
- (id<MTLRenderPipelineState>)ycbcrTargetState:(MTLRenderPipelineDescriptor *)d
                                       options:(MTLPipelineOption)o
                                    reflection:(MTLAutoreleasedRenderPipelineReflection *)reflection
                                         error:(NSError **)error
{
    if (rmRenderArchiveMissed(d, o, error))
        return nil;
    o &= ~MTLPipelineOptionFailOnBinaryArchiveMiss;
    MTLRenderPipelineDescriptor *made = [d copy];
    for (NSUInteger c = 0; c < 8; c++) {
        MTLRenderPipelineColorAttachmentDescriptor *a = d.colorAttachments[c];
        const uint32_t format = uint32_t(a.pixelFormat);
        if (!raw::ycbcrCodeFormat(format))
            continue;
        constexpr MTLColorWriteMask kRGB = MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue;
        const char *why = raw::ycbcrTargetRefusal(format);
        if (!why && (a.blendingEnabled || (a.writeMask & kRGB) != kRGB))
            why = "a blended or channel-masked YCbCr code attachment";
        if (why) {
            SWITCH_REFUSE_DETAIL("raw", "state.ycbcr", format,
                                 "a state of a format %u color attachment, left to m2v without a raw compile: %s", format,
                                 why);
            RMPipeline *p = [RMPipeline new];
            p->_device = self;
            p.label = d.label;
            p->_twin = [gRM->fallback newRenderPipelineStateWithDescriptor:twinDescriptor(d)
                                                                   options:o
                                                                reflection:reflection
                                                                     error:error];
            return p->_twin ? (id<MTLRenderPipelineState>)p : nil;
        }
        made.colorAttachments[c].pixelFormat = MTLPixelFormat(raw::kYcbcrTargetStandIn);
    }
    RMPipeline *p = rmPipelineFor(self, made, o, error);
    if (!p->_raw && !p->_asked) {
        if (error)
            *error = [NSError errorWithDomain:MTLLibraryErrorDomain
                                         code:MTLLibraryErrorCompileFailure
                                     userInfo:@{
                                         NSLocalizedDescriptionKey : @"the raw path made no state for a YCbCr code "
                                                                     @"target, and m2v renders none"
                                     }];
        return nil;
    }
    MTLRenderPipelineDescriptor *twin = twinDescriptor(made);
    if (reflection)
        p->_twin = [gRM->fallback newRenderPipelineStateWithDescriptor:twin
                                                               options:o
                                                            reflection:reflection
                                                                 error:error];
    else if (native_bridge_metal_render_pipeline_lazy)
        p->_twin = native_bridge_metal_render_pipeline_lazy(gRM->fallback, twin, o);
    if (error)
        *error = nil;
    return (id<MTLRenderPipelineState>)p;
}

@end
