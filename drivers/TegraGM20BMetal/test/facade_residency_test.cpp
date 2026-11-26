// SPDX-License-Identifier: MIT
// Residency sets (TegraGM20BMetalDeviceBase.mm gm20bNameResidencySets), through the facade over the kernel model: a
// program may reach a set's allocations by GPU address alone, so a job of a command buffer that uses a set holding a
// host-visible allocation is host-written and External, from the command buffer's set and from its queue's. A set of
// Private allocations alone names nothing at a range's seal, however many it holds: the set takes its host-visible
// allocations once, at its commit.
#include "facade_harness.h"

#include <gtest/gtest.h>

namespace {

using namespace harness;

constexpr NSUInteger kLength = 4096;

long counted(id<MTLDevice> device, NSString *key)
{
    return [counters(device)[key] longValue];
}

id<MTLResidencySet> setOf(id<MTLDevice> device, NSArray<id<MTLAllocation>> *allocations)
{
    MTLResidencySetDescriptor *d = [MTLResidencySetDescriptor new];
    NSError *error = nil;
    id<MTLResidencySet> set = [device newResidencySetWithDescriptor:d error:&error];
    EXPECT_NE(set, nil);
    for (id<MTLAllocation> allocation in allocations)
        [set addAllocation:allocation];
    [set commit];
    return set;
}

// A fill of `target` alone, its command buffer using `set` (nil: none)
kernel_model::Job filled(id<MTLCommandQueue> queue, id<MTLBuffer> target, id<MTLResidencySet> set)
{
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    if (set)
        [cb useResidencySet:set];
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b fillBuffer:target range:NSMakeRange(0, kLength) value:0x11];
    [b endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted);
    return kernel_model::model.jobs.back();
}

bool coherent(const kernel_model::Job &job)
{
    return job.hostWritten && (job.flags & TegraGM20BJobExternal);
}

TEST(GM20BFacadeResidency, ASetHoldingASharedBufferMakesItsJobsHostWrittenAndExternal)
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_NE(device, nil);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLBuffer> target = [device newBufferWithLength:kLength options:MTLResourceStorageModePrivate];
    id<MTLBuffer> shared = [device newBufferWithLength:kLength options:MTLResourceStorageModeShared];
    EXPECT_FALSE(coherent(filled(queue, target, nil))) << "the control: a private fill alone";
    id<MTLResidencySet> set = setOf(device, @[ shared, target ]);
    const long uses = counted(device, @"residency_set_uses"), named = counted(device, @"residency_named");
    EXPECT_TRUE(coherent(filled(queue, target, set))) << "a set's Shared buffer reached by GPU address alone";
    EXPECT_EQ(counted(device, @"residency_set_uses") - uses, 1);
    EXPECT_EQ(counted(device, @"residency_named") - named, 1) << "the Shared buffer alone";

    id<MTLCommandQueue> resident = [device newCommandQueue];
    [resident addResidencySet:set];
    EXPECT_TRUE(coherent(filled(resident, target, nil))) << "the queue's set";
    [resident removeResidencySet:set];
    EXPECT_FALSE(coherent(filled(resident, target, nil))) << "removed from the queue";
}

TEST(GM20BFacadeResidency, ASetOfPrivateAllocationsNamesNothingAtASeal)
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_NE(device, nil);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    NSMutableArray<id<MTLAllocation>> *privates = [NSMutableArray array];
    for (int i = 0; i < 256; i++)
        [privates addObject:[device newBufferWithLength:kLength options:MTLResourceStorageModePrivate]];
    id<MTLResidencySet> set = setOf(device, privates);
    const long named = counted(device, @"residency_named");
    EXPECT_FALSE(coherent(filled(queue, (id<MTLBuffer>)privates[0], set)));
    EXPECT_EQ(counted(device, @"residency_named") - named, 0) << "a seal walked the set's Private allocations";
}

}  // namespace
