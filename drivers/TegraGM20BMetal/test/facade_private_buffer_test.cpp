// SPDX-License-Identifier: MIT
// A buffer of private storage, which no CPU reads or writes, takes no CPU lease as it is made: a lease reaches the
// kernel's whole BO, the pool chunk the buffer lies in, and takes that chunk out of the device's resource set for good
// (SwitchGpuResourceSet.cpp cpu_reached), so each later job names every resource in it as a use of its own. A shared
// buffer is L2-cached write-combined memory: it takes its lease, a job that reads it is host-written (the kernel drops
// the L2's lines first) and one that writes it is External (its lines reach DRAM before the fence). Only HW Test's probe
// makes one L2-bypassing, which stays in the resource set. A shared fallback into pool memory takes its lease, and a
// private buffer placed in a chunk a host-visible buffer touched before keeps its lease too.
#include "facade_harness.h"
#include "bazel/testing/fresh_process.h"

#include <gtest/gtest.h>

@interface NSObject (SwitchSharedBypassProbe)
- (NSString *)switchMetalSharedBypassesL2:(NSNumber *)on;
@end

namespace {

using namespace harness;

const kernel_model::BoState *boAt(uint64_t va)
{
    for (const auto &[at, bound] : kernel_model::model.binds)
        if (va >= at && va - at < bound.second) {
            const auto it = kernel_model::model.bos.find(bound.first);
            return it == kernel_model::model.bos.end() ? nullptr : &it->second;
        }
    return nullptr;
}

void aPrivateBufferTakesNoLease()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    // over half a pool chunk: a chunk of its own, made for it
    constexpr NSUInteger kLength = 10u << 20;
    id<MTLBuffer> made = [device newBufferWithLength:kLength options:MTLResourceStorageModePrivate];
    ASSERT_TRUE(made != nil);
    EXPECT_EQ(made.contents, nullptr);
    const kernel_model::BoState *chunk = boAt(((SwitchMetalBuffer *)made)->_raw.va);
    ASSERT_NE(chunk, nullptr);
    EXPECT_FALSE(chunk->cpuReached) << "the private buffer's chunk left the resource set";
    std::vector<uint8_t> bytes(kLength, 0x5a);
    id<MTLBuffer> written =
        [device newBufferWithBytes:bytes.data() length:kLength options:MTLResourceStorageModePrivate];
    ASSERT_TRUE(written != nil);
    const kernel_model::BoState *copied = boAt(((SwitchMetalBuffer *)written)->_raw.va);
    ASSERT_NE(copied, nullptr);
    EXPECT_TRUE(copied->cpuReached) << "the CPU copies a private buffer's initial bytes under a lease";
    id<MTLBuffer> shared = [device newBufferWithLength:kLength options:MTLResourceStorageModeShared];
    ASSERT_TRUE(shared != nil);
    EXPECT_NE(shared.contents, nullptr);
    EXPECT_TRUE(((SwitchMetalBuffer *)shared)->_raw.wc);
    EXPECT_FALSE(((SwitchMetalBuffer *)shared)->_raw.gpuVolatile) << "a shared buffer is read around the GPU's L2";
}

// The job of one blit encoder, committed and waited for
const kernel_model::Job &blitted(id<MTLDevice> device, void (^encode)(id<MTLBlitCommandEncoder>))
{
    id<MTLCommandBuffer> commands = [[device newCommandQueue] commandBuffer];
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    encode(blit);
    [blit endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    EXPECT_EQ(commands.status, MTLCommandBufferStatusCompleted);
    return kernel_model::model.jobs.back();
}

void aSharedBufferIsReadAndWrittenThroughTheL2()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    constexpr NSUInteger kLength = 64u << 10;
    id<MTLBuffer> shared = [device newBufferWithLength:kLength options:MTLResourceStorageModeShared];
    id<MTLBuffer> priv = [device newBufferWithLength:kLength options:MTLResourceStorageModePrivate];
    ASSERT_TRUE(shared != nil && priv != nil);
    ASSERT_FALSE(((SwitchMetalBuffer *)shared)->_raw.gpuVolatile);
    memset(shared.contents, 0x5a, kLength);
    const kernel_model::Job &read = blitted(device, ^(id<MTLBlitCommandEncoder> b) {
        [b copyFromBuffer:shared sourceOffset:0 toBuffer:priv destinationOffset:0 size:kLength];
    });
    EXPECT_TRUE(read.hostWritten) << "a job reading a shared buffer drops the L2's lines of the CPU's writes first";
    const kernel_model::Job &written = blitted(device, ^(id<MTLBlitCommandEncoder> b) {
        [b fillBuffer:shared range:NSMakeRange(0, kLength) value:0x33];
    });
    EXPECT_NE(written.flags & TegraGM20BJobExternal, 0u)
        << "a job writing a shared buffer flushes the L2 for its reader";
}

void theProbeMakesAnL2BypassingSharedBuffer()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    NSObject *probe = (NSObject *)device;
    ASSERT_TRUE([probe respondsToSelector:@selector(switchMetalSharedBypassesL2:)]);
    EXPECT_TRUE([[probe switchMetalSharedBypassesL2:@YES] isEqualToString:@"0"]);
    id<MTLBuffer> streamed = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    EXPECT_TRUE([[probe switchMetalSharedBypassesL2:@NO] isEqualToString:@"1"]);
    id<MTLBuffer> cached = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    ASSERT_TRUE(streamed != nil && cached != nil);
    EXPECT_TRUE(((SwitchMetalBuffer *)streamed)->_raw.gpuVolatile);
    EXPECT_FALSE(((SwitchMetalBuffer *)cached)->_raw.gpuVolatile);
    const kernel_model::BoState *bo = boAt(((SwitchMetalBuffer *)streamed)->_raw.va);
    ASSERT_NE(bo, nullptr);
    EXPECT_FALSE(bo->cpuReached) << "an L2-bypassing shared buffer stays in the resource set";
}

std::atomic<bool> gRefuseCoherent{false};
std::atomic<uint32_t> gCpuAcquires{0};

int hookCalls(uint32_t method, const void *input)
{
    if (method == TegraGM20BMethodBoAlloc && gRefuseCoherent.load(std::memory_order_acquire) &&
        (static_cast<const TegraGM20BAbiBoAlloc *>(input)->flags & TegraGM20BBoCoherent))
        return kIOReturnNoMemory;
    if (method == TegraGM20BMethodCpuAcquire)
        gCpuAcquires.fetch_add(1, std::memory_order_relaxed);
    return KERN_SUCCESS;
}

void aPrivateBufferInAHostTouchedChunkKeepsItsLease()
{
    kernel_model::HooksCleared clear;
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    constexpr NSUInteger kSmall = 4096;
    id<MTLBuffer> first = [device newBufferWithLength:kSmall options:MTLResourceStorageModePrivate];
    ASSERT_TRUE(first != nil);
    const kernel_model::BoState *pool = boAt(((SwitchMetalBuffer *)first)->_raw.va);
    ASSERT_NE(pool, nullptr);
    EXPECT_FALSE(pool->cpuReached) << "the pool chunk left the resource set on a private buffer";
    // Refuse coherent BoAlloc so the shared buffer falls back into the same pool chunk
    gRefuseCoherent.store(true, std::memory_order_release);
    kernel_model::arriving.store(hookCalls, std::memory_order_release);
    @autoreleasepool {
        id<MTLBuffer> shared = [device newBufferWithLength:kSmall options:MTLResourceStorageModeShared];
        ASSERT_TRUE(shared != nil);
        ASSERT_EQ(boAt(((SwitchMetalBuffer *)shared)->_raw.va), pool);
        EXPECT_TRUE(pool->cpuReached);
    }
    gRefuseCoherent.store(false, std::memory_order_release);
    gCpuAcquires.store(0, std::memory_order_relaxed);
    id<MTLBuffer> reused = [device newBufferWithLength:kSmall options:MTLResourceStorageModePrivate];
    ASSERT_TRUE(reused != nil);
    EXPECT_EQ(boAt(((SwitchMetalBuffer *)reused)->_raw.va), pool);
    EXPECT_EQ(gCpuAcquires.load(std::memory_order_relaxed), 1u)
        << "a private buffer placed in a host-touched pool chunk must still clean";
}

}  // namespace

TEST(GM20BFacadePrivateBuffer, APrivateBufferTakesNoLeaseAndItsChunkStaysInTheSet)
{
    switch_ios::test::ExpectPassesInFreshProcess(aPrivateBufferTakesNoLease);
}

TEST(GM20BFacadePrivateBuffer, APrivateBufferInAHostTouchedChunkKeepsItsLease)
{
    switch_ios::test::ExpectPassesInFreshProcess(aPrivateBufferInAHostTouchedChunkKeepsItsLease);
}

TEST(GM20BFacadePrivateBuffer, ASharedBufferIsReadAndWrittenThroughTheL2)
{
    switch_ios::test::ExpectPassesInFreshProcess(aSharedBufferIsReadAndWrittenThroughTheL2);
}

TEST(GM20BFacadePrivateBuffer, TheProbeMakesAnL2BypassingSharedBuffer)
{
    switch_ios::test::ExpectPassesInFreshProcess(theProbeMakesAnL2BypassingSharedBuffer);
}
