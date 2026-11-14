// SPDX-License-Identifier: MIT
// Core Animation shaped command buffers through the raw facade over the kernel model: the words each job carries, written
// to the test's outputs (words.txt) for a comparison with another build's, and one hash of them.
#include "raw_job_words.h"
#include "raw_scale_frames.h"

#include <gtest/gtest.h>
#include <cstdlib>
#include <fstream>

namespace {

using namespace harness;

struct Opened {
    id<MTLDevice> device;
    id<MTLRenderPipelineState> state;
};

Opened openScale()
{
    const std::string pack = writePack(packKey(MTLPixelFormatBGRA8Unorm));
    const std::string metallib = writeMetallib();
    id<MTLDevice> device = open(pack);
    if (!device)
        return {};
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(metallib.c_str())] error:&error];
    id<MTLRenderPipelineState> state =
        [device newRenderPipelineStateWithDescriptor:descriptor(library, MTLPixelFormatBGRA8Unorm) error:&error];
    return {device, state};
}

std::string outputs()
{
    const char *at = getenv("TEST_UNDECLARED_OUTPUTS_DIR");
    return at ? at : dir();
}

TEST(RawFacadeCommit, ScaleShapedJobWords)
{
    const Opened opened = openScale();
    ASSERT_TRUE(opened.device != nil && opened.state != nil);
    id<MTLCommandQueue> queue = [opened.device newCommandQueue];
    const scale_frames::Shape shapes[] = {{1, 6, 1}, {15, 90, 4}, {12, 134, 3}, {7, 20, 2}};
    std::vector<std::vector<id<MTLTexture>>> into;
    for (const auto &shape : shapes)
        into.push_back(scale_frames::targets(opened.device, shape));
    std::vector<job_words::Run> runs;
    const uint64_t mark = job_words::handleMark();
    for (uint32_t frame = 0; frame < 3; frame++)
        for (size_t s = 0; s < std::size(shapes); s++) {
            id<MTLCommandBuffer> cb = scale_frames::record(queue, opened.state, into[s], shapes[s], frame);
            runs.push_back(job_words::run(cb));
            [cb commit];
            [cb waitUntilCompleted];
            EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted);
            EXPECT_EQ(std::string([[(id)cb performSelector:@selector(switchRawPath)] UTF8String]), "raw");
        }
    const std::string words = job_words::words(runs, mark);
    std::ofstream(outputs() + "/words.txt") << words;
    printf("scale words: %zu bytes, hash %016llx\n", words.size(), (unsigned long long)job_words::fnv(words));
    EXPECT_FALSE(words.empty());
}

}  // namespace
