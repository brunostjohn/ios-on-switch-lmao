// SPDX-License-Identifier: MIT
// Core Animation's catch-all state named by its label (ca_catch_all.h), and the rates line's count of its draws (cag=)
// on the facade over the kernel model: a state labelled as the catch-all counts each draw, one labelled as a
// specialised state of the same key none.
#include "scale_frames.h"
#include "../ca_catch_all.h"

#include <gtest/gtest.h>
#include <cstdlib>
#include <string>

namespace {

TEST(GM20BCaCatchAll, TheFragmentSegmentsGenericFlagNamesIt)
{
    EXPECT_TRUE(gm20b::caCatchAll("com.apple.coreanimation.PBGRAXmw_A3Xghfc"));
    EXPECT_TRUE(gm20b::caCatchAll("com.apple.coreanimation.PRGhAXm_A2Xghfc"));
    EXPECT_TRUE(gm20b::caCatchAll("com.apple.coreanimation.PBGRAXm_A2Xgf"));
    EXPECT_FALSE(gm20b::caCatchAll("com.apple.coreanimation.PBGRAXm_TimgA2Xhf_Isrc"));
    EXPECT_FALSE(gm20b::caCatchAll("com.apple.coreanimation.PRGhAXm_TimgBdfuA2Xhvfu_Irsd"));
    EXPECT_FALSE(gm20b::caCatchAll("com.apple.coreanimation.PBGRAXm_TgsaA2Xhf_Isrc"));
    EXPECT_FALSE(gm20b::caCatchAll("com.apple.coreanimation.PBGRAXm_TimgA2Xhf_Igra"));
    EXPECT_FALSE(gm20b::caCatchAll("PBGRAXmw_A3Xghfc"));
    EXPECT_FALSE(gm20b::caCatchAll(nullptr));
}

uint64_t catchAllDraws(const std::string &line)
{
    const size_t at = line.find(" cag=");
    return at == std::string::npos ? UINT64_MAX : strtoull(line.c_str() + at + 5, nullptr, 10);
}

TEST(GM20BCaCatchAll, TheRatesLineCountsEachDrawOfTheCatchAllAlone)
{
    id<MTLDevice> device = scale_frames::opened().device;
    ASSERT_TRUE(device != nil);
    NSError *error = nil;
    id<MTLLibrary> library =
        [device newLibraryWithURL:[NSURL fileURLWithPath:@(harness::writeMetallib().c_str())] error:&error];
    ASSERT_TRUE(library != nil);
    MTLRenderPipelineDescriptor *d = harness::descriptor(library, MTLPixelFormatBGRA8Unorm);
    d.label = @"com.apple.coreanimation.PBGRAXm_A2Xghfc";
    id<MTLRenderPipelineState> catchAll = [device newRenderPipelineStateWithDescriptor:d error:&error];
    d.label = @"com.apple.coreanimation.PBGRAXm_TimgA2Xhf_Isrc";
    id<MTLRenderPipelineState> specialised = [device newRenderPipelineStateWithDescriptor:d error:&error];
    ASSERT_TRUE(catchAll != nil && specialised != nil);
    const std::vector<id<MTLTexture>> into = scale_frames::targets(device, {1, 1, 1});
    id<MTLCommandBuffer> cb = [[device newCommandQueue] commandBuffer];
    scale_frames::pass(cb, catchAll, into, {1, 3, 1}, 0, 0);
    scale_frames::pass(cb, specialised, into, {1, 2, 1}, 0, 0);
    [cb commit];
    [cb waitUntilCompleted];
    ASSERT_EQ(cb.status, MTLCommandBufferStatusCompleted);
    char line[4096] = {};
    ASSERT_GT(metal_rates(line, sizeof line), 0u);
    EXPECT_EQ(catchAllDraws(line), 3u) << line;
}

}  // namespace
