// SPDX-License-Identifier: MIT
// A depth's zcull region kept from the pass that stores it to the pass after a blit that loads it (zcull_planes.h), through
// the facade over the kernel model with the GPU's zcull layout: the submit worker encodes the storing pass ahead of the
// commit (TegraGM20BMetalPipeline.mm), before the loading pass is recorded, as HW Test gpu.gm20b.zcull's `loaded` arm
// records them, and the loading pass loads the region all the same.
#include "scale_frames.h"
#include "TegraGM20BZcullABI.h"

#include <gtest/gtest.h>
#include <chrono>
#include <thread>

namespace {

using scale_frames::opened;

constexpr TegraGM20BAbiZcullInfo kX1Zcull = {32, 32, 1024, 2048, 32, 32, 192, 16, 32, 64, 0x1500, 0x1000};

const scale_frames::Shape kShape{4, 24, 4};

const scale_frames::Opened &openedWithZcull()
{
    static const bool answered = [] {
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        kernel_model::model.otherMethod = [](uint32_t method, void *output) -> kern_return_t {
            if (method != TegraGM20BMethodZcullInfo)
                return kIOReturnUnsupported;
            *static_cast<TegraGM20BAbiZcullInfo *>(output) = kX1Zcull;
            return KERN_SUCCESS;
        };
        return true;
    }();
    (void)answered;
    return opened();
}

id<MTLTexture> depthTexture()
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                 width:kShape.width
                                                                                height:kShape.height
                                                                             mipmapped:NO];
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageRenderTarget;
    return [openedWithZcull().device newTextureWithDescriptor:d];
}

void depthPass(id<MTLCommandBuffer> cb, id<MTLTexture> colour, id<MTLTexture> depth, bool first)
{
    MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
    d.colorAttachments[0].texture = colour;
    d.colorAttachments[0].loadAction = MTLLoadActionLoad;
    d.colorAttachments[0].storeAction = MTLStoreActionStore;
    d.depthAttachment.texture = depth;
    d.depthAttachment.loadAction = first ? MTLLoadActionClear : MTLLoadActionLoad;
    d.depthAttachment.clearDepth = 1;
    d.depthAttachment.storeAction = MTLStoreActionStore;
    [[cb renderCommandEncoderWithDescriptor:d] endEncoding];
}

long long counter(NSString *name)
{
    return [harness::counters(openedWithZcull().device)[name] longLongValue];
}

// Core Animation shaped passes enough for the worker to be handed the command buffer, then a pass that clears and stores a
// depth, a blit that writes another buffer and more of those passes, and once the worker has encoded all that ahead, a
// pass that loads the depth: the commit's range is not cut where the encode ahead ended, before the loading pass
TEST(GM20BFacadeZcullAhead, APassAfterABlitLoadsTheRegionAPassEncodedAheadStored)
{
    ASSERT_TRUE(openedWithZcull().device != nil);
    id<MTLCommandQueue> queue = [openedWithZcull().device newCommandQueue];
    const auto into = scale_frames::targets(openedWithZcull().device, kShape);
    id<MTLTexture> depth = depthTexture();
    id<MTLBuffer> between = [openedWithZcull().device newBufferWithLength:256 options:MTLResourceStorageModeShared];
    ASSERT_TRUE(depth != nil && between != nil);
    const long long passes = counter(@"zcull_passes"), loads = counter(@"zcull_loads");
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    for (int p = 0; p < kShape.passes; p++) {
        scale_frames::pass(cb, openedWithZcull().state, into, kShape, p, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    depthPass(cb, into[0], depth, true);
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit fillBuffer:between range:NSMakeRange(0, 256) value:0];
    [blit endEncoding];
    for (int p = 0; p < kShape.passes; p++)
        scale_frames::pass(cb, openedWithZcull().state, into, kShape, p, 1);
    EXPECT_EQ(scale_frames::waitAhead(cb), 0u);
    depthPass(cb, into[0], depth, false);
    [cb commit];
    [cb waitUntilCompleted];
    ASSERT_EQ(cb.status, MTLCommandBufferStatusCompleted);
    NSDictionary *p = ((NSDictionary * (*)(id, SEL)) objc_msgSend)((id)cb, sel_registerName("switchMetalPipelined"));
    ASSERT_NE(p, nil);
    EXPECT_GE([p[@"pipelined"] intValue], kShape.passes + 2) << [[p description] UTF8String];
    EXPECT_EQ(counter(@"zcull_passes") - passes, 2);
    EXPECT_EQ(counter(@"zcull_loads") - loads, 1);
}

}  // namespace
