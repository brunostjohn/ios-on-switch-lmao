// SPDX-License-Identifier: MIT
// hw_test_app's 2D-engine probe ops through the raw facade (RawMetalProbe.mm switchRawProbeEngine:): the textures an op
// reads and writes are uses of its blit encoder in the command buffer's plan, as a copy's are, so a new texture's first
// contents (RawMetalFresh.mm) are written ahead of that encoder, never over what the op wrote.
#include "raw_facade_harness.h"

namespace {

id<MTLTexture> target()
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                 width:16
                                                                                height:16
                                                                             mipmapped:NO];
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    return [facade::device() newTextureWithDescriptor:d];
}

NSString *engine(id<MTLCommandBuffer> cb, NSDictionary *op)
{
    return ((NSString * (*)(id, SEL, NSDictionary *)) objc_msgSend)(cb, NSSelectorFromString(@"switchRawProbeEngine:"),
                                                                     op);
}

uint8_t access(RMCommandPlan &plan, id<MTLTexture> t)
{
    plan.prepareRange(0, plan.size());
    uint8_t found = 0;
    plan.forEachResource(0, plan.size(), RMResourceKind::Texture, [&](const RMRangeResource &use) {
        if (use.object == (__bridge void *)t)
            found |= use.access;
    });
    return found;
}

TEST(FacadeProbe, AScaleNamesItsTexturesInItsBlitEncoder)
{
    @autoreleasepool {
        id<MTLTexture> read = target(), written = target();
        ASSERT_NE(read, nil);
        ASSERT_NE(written, nil);
        id<MTLCommandBuffer> cb = [[facade::device() newCommandQueue] commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        const uint64_t step = 1ull << 32;
        EXPECT_EQ(engine(cb, @{
                      @"kind" : @"scale",
                      @"read" : read,
                      @"written" : written,
                      @"du" : @(step),
                      @"dv" : @(step),
                      @"sx" : @(step / 2),
                      @"sy" : @(step / 2),
                      @"corner" : @1,
                  }),
                  nil);
        [blit endEncoding];
        RMCommandPlan &plan = ((RMCommandBuffer *)cb)->_plan;
        EXPECT_TRUE(access(plan, written) & raw::ResourceUse::Write);
        EXPECT_TRUE(access(plan, read) & raw::ResourceUse::Read);
        EXPECT_FALSE(access(plan, read) & raw::ResourceUse::Write);
    }
}

TEST(FacadeProbe, AFillNamesItsTarget)
{
    @autoreleasepool {
        id<MTLTexture> written = target();
        ASSERT_NE(written, nil);
        id<MTLCommandBuffer> cb = [[facade::device() newCommandQueue] commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        EXPECT_EQ(engine(cb, @{
                      @"kind" : @"fill",
                      @"written" : written,
                      @"dx" : @1,
                      @"dy" : @1,
                      @"dw" : @14,
                      @"dh" : @14,
                      @"colorFormat" : @0xcf,
                      @"color0" : @0xff336699u,
                  }),
                  nil);
        [blit endEncoding];
        EXPECT_TRUE(access(((RMCommandBuffer *)cb)->_plan, written) & raw::ResourceUse::Write);
    }
}

}  // namespace
