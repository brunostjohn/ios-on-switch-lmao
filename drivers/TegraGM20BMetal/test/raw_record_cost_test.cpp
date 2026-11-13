// SPDX-License-Identifier: MIT
// What recording costs the thread that records, through the raw facade over the kernel model: a compositor scale frame
// (boot 21: 15 passes, 30 render target switches, 90 draws, 55 textures of other processes) and its parts, each draw
// setting its vertices, transform and colour by setBytes, a texture and sampler by Core Animation's call and a scissor
// of its own; the same frame at twice the draws; encoders with no draws; draws with no state set between them. Per
// scenario: time, instructions and cycles of the process, and the recording thread's Objective-C messages, locks and
// allocations (record_count.c). The counts are the test; times are printed.
#include "raw_facade_harness.h"

#include <gtest/gtest.h>
#include <libproc.h>
#include <mach/mach_time.h>
#include <sys/resource.h>
#include <algorithm>
#include <cstdio>
#include <functional>

extern "C" {
struct record_count {
    uint64_t sends, locks, mallocs, frees;
};
void record_count_start(void);
void record_count_stop(struct record_count *out);
}

@protocol RecordCostCoreAnimationSPI
- (void)setFragmentTexture:(id<MTLTexture>)texture
            atTextureIndex:(NSUInteger)textureIndex
              samplerState:(id<MTLSamplerState>)sampler
            atSamplerIndex:(NSUInteger)samplerIndex;
@end

namespace {

using namespace harness;

constexpr int kEncoders = 30, kTargets = 15, kTextures = 55, kDraws = 90;
constexpr int kWarm = 8, kRounds = 40;

struct Sample {
    double us = 0, instructions = 0, cycles = 0;
    record_count counts{};
};

struct Bench {
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLRenderPipelineState> state;
    std::vector<id<MTLTexture>> targets, textures;
    id<MTLSamplerState> sampler;
};

std::vector<raw::PackEntry> compositorEntries()
{
    raw::PackEntry texture = {};
    texture.offset = 0;
    texture.kind = raw::EntryTexture;
    texture.stage = 2;
    texture.slot = 0;
    texture.samplerKind = raw::SamplerSlot;
    texture.sampler = 0;
    raw::PackEntry transform = {};
    transform.offset = 16;
    transform.kind = raw::EntryUbo;
    transform.stage = 1;
    transform.slot = 2;
    raw::PackEntry colour = {};
    colour.offset = 32;
    colour.kind = raw::EntryUbo;
    colour.stage = 2;
    colour.slot = 0;
    return {texture, transform, colour};
}

id<MTLTexture> texture(id<MTLDevice> device, uint32_t width, uint32_t height, MTLTextureUsage usage)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                 width:width
                                                                                height:height
                                                                             mipmapped:NO];
    d.storageMode = MTLStorageModePrivate;
    d.usage = usage;
    return [device newTextureWithDescriptor:d];
}

Bench &bench()
{
    static Bench b = [] {
        Bench made;
        const std::string pack = writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, compositorEntries());
        const std::string metallib = writeMetallib();
        made.device = open(pack);
        if (!made.device)
            return made;
        NSError *error = nil;
        id<MTLLibrary> library = [made.device newLibraryWithURL:[NSURL fileURLWithPath:@(metallib.c_str())]
                                                          error:&error];
        made.state = [made.device newRenderPipelineStateWithDescriptor:descriptor(library, MTLPixelFormatBGRA8Unorm)
                                                                 error:&error];
        made.queue = [made.device newCommandQueue];
        for (int i = 0; i < kTargets; i++)
            made.targets.push_back(
                texture(made.device, 320, 180, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead));
        for (int i = 0; i < kTextures; i++)
            made.textures.push_back(texture(made.device, 64, 64, MTLTextureUsageShaderRead));
        made.sampler = [made.device newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]];
        return made;
    }();
    return b;
}

id<MTLRenderCommandEncoder> begin(id<MTLCommandBuffer> cb, id<MTLTexture> target, bool load)
{
    MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
    d.colorAttachments[0].texture = target;
    d.colorAttachments[0].loadAction = load ? MTLLoadActionLoad : MTLLoadActionClear;
    d.colorAttachments[0].storeAction = MTLStoreActionStore;
    return [cb renderCommandEncoderWithDescriptor:d];
}

void compositorDraw(id<MTLRenderCommandEncoder> e, const Bench &b, int draw, uint32_t frame)
{
    const float x = float((draw * 37 + frame) % 97) / 97.0f - 0.5f, y = float(draw * 13 % 89) / 89.0f;
    const float vertices[16] = {x, y, 0, 1, x + 0.25f, y, 0, 1, x, y + 0.25f, 0, 1, x + 0.25f, y + 0.25f, 0, 1};
    const float transform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, float(frame % 7) * 0.01f, 0, 0, 1};
    const float colour[4] = {float(draw % 5) / 5, 0.5f, 0.5f, 1};
    [e setVertexBytes:vertices length:sizeof vertices atIndex:0];
    [e setVertexBytes:transform length:sizeof transform atIndex:2];
    [e setFragmentBytes:colour length:sizeof colour atIndex:0];
    [(id<RecordCostCoreAnimationSPI>)e setFragmentTexture:b.textures[size_t(draw) % b.textures.size()]
                                           atTextureIndex:0
                                             samplerState:b.sampler
                                           atSamplerIndex:0];
    [e setScissorRect:MTLScissorRect{NSUInteger(draw * 17 % 120), NSUInteger(draw * 11 % 60), 200, 120}];
    [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
}

void compositorFrame(id<MTLCommandBuffer> cb, const Bench &b, int draws, uint32_t frame)
{
    int drawn = 0;
    for (int p = 0; p < kEncoders; p++) {
        id<MTLRenderCommandEncoder> e = begin(cb, b.targets[size_t(p) % b.targets.size()], p >= kTargets);
        [e setRenderPipelineState:b.state];
        const int mine = draws / kEncoders + (p < draws % kEncoders ? 1 : 0);
        for (int i = 0; i < mine; i++)
            compositorDraw(e, b, drawn++, frame);
        [e endEncoding];
    }
}

void emptyEncoders(id<MTLCommandBuffer> cb, const Bench &b)
{
    for (int p = 0; p < kEncoders; p++) {
        id<MTLRenderCommandEncoder> e = begin(cb, b.targets[size_t(p) % b.targets.size()], p >= kTargets);
        [e setRenderPipelineState:b.state];
        [e endEncoding];
    }
}

void repeatedDraws(id<MTLCommandBuffer> cb, const Bench &b, int repeats)
{
    id<MTLRenderCommandEncoder> e = begin(cb, b.targets[0], false);
    [e setRenderPipelineState:b.state];
    compositorDraw(e, b, 0, 0);
    for (int i = 0; i < repeats; i++)
        [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [e endEncoding];
}

rusage_info_v4 usage()
{
    rusage_info_v4 u = {};
    proc_pid_rusage(getpid(), RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t *>(&u));
    return u;
}

double ticksToUs(uint64_t ticks)
{
    static const double perTick = [] {
        mach_timebase_info_data_t tb{};
        mach_timebase_info(&tb);
        return double(tb.numer) / double(tb.denom) / 1000.0;
    }();
    return double(ticks) * perTick;
}

// The median round of `record` (each into a command buffer of its own, committed and completed outside the window)
Sample measure(const std::function<void(id<MTLCommandBuffer>, uint32_t)> &record)
{
    Bench &b = bench();
    std::vector<Sample> samples;
    for (int round = 0; round < kWarm + kRounds; round++)
        @autoreleasepool {
            id<MTLCommandBuffer> cb = [b.queue commandBuffer];
            Sample s;
            const rusage_info_v4 before = usage();
            record_count_start();
            const uint64_t t = mach_absolute_time();
            record(cb, uint32_t(round));
            const uint64_t ticks = mach_absolute_time() - t;
            record_count_stop(&s.counts);
            const rusage_info_v4 after = usage();
            s.us = ticksToUs(ticks);
            s.instructions = double(after.ri_instructions - before.ri_instructions);
            s.cycles = double(after.ri_cycles - before.ri_cycles);
            [cb commit];
            [cb waitUntilCompleted];
            EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted);
            if (round >= kWarm)
                samples.push_back(s);
        }
    std::sort(samples.begin(), samples.end(), [](const Sample &a, const Sample &c) {
        return a.us < c.us;
    });
    return samples[samples.size() / 2];
}

void print(const char *name, const Sample &s, double per, const char *unit)
{
    printf("record cost %-22s %8.1f us %10.0f instr %10.0f cycles | sends %6llu locks %4llu mallocs %4llu frees %4llu | "
           "per %s: %.3f us %.0f instr %.2f sends %.2f locks %.2f mallocs\n",
           name, s.us, s.instructions, s.cycles, (unsigned long long)s.counts.sends,
           (unsigned long long)s.counts.locks, (unsigned long long)s.counts.mallocs, (unsigned long long)s.counts.frees,
           unit, s.us / per, s.instructions / per, double(s.counts.sends) / per, double(s.counts.locks) / per,
           double(s.counts.mallocs) / per);
}

Sample difference(const Sample &a, const Sample &b)
{
    Sample d;
    d.us = a.us - b.us;
    d.instructions = a.instructions - b.instructions;
    d.cycles = a.cycles - b.cycles;
    d.counts = {a.counts.sends - b.counts.sends, a.counts.locks - b.counts.locks, a.counts.mallocs - b.counts.mallocs,
                a.counts.frees - b.counts.frees};
    return d;
}

TEST(RawRecordCost, CompositorScaleFrame)
{
    Bench &b = bench();
    ASSERT_TRUE(b.device != nil && b.state != nil);
    const Sample frame = measure([&](id<MTLCommandBuffer> cb, uint32_t f) {
        compositorFrame(cb, b, kDraws, f);
    });
    const Sample doubled = measure([&](id<MTLCommandBuffer> cb, uint32_t f) {
        compositorFrame(cb, b, 2 * kDraws, f);
    });
    const Sample encoders = measure([&](id<MTLCommandBuffer> cb, uint32_t) {
        emptyEncoders(cb, b);
    });
    const Sample one = measure([&](id<MTLCommandBuffer> cb, uint32_t) {
        repeatedDraws(cb, b, 0);
    });
    const Sample repeated = measure([&](id<MTLCommandBuffer> cb, uint32_t) {
        repeatedDraws(cb, b, 300);
    });
    print("frame 30 enc 90 draws", frame, 1, "frame");
    print("changed-state draw", difference(doubled, frame), kDraws, "draw");
    print("empty encoder", encoders, kEncoders, "encoder");
    print("unchanged-state draw", difference(repeated, one), 300, "draw");
    EXPECT_GT(frame.counts.sends, 0u);
}

}  // namespace
