// SPDX-License-Identifier: MIT
// Guards on why an MTLFence needs no push of its own on the raw path (RawMetalFences.mm), which no host model runs: every
// encoder the raw path opens begins with a drain, the raw encode emits nothing for a fence, and m2v's replay of a refused
// encoder gets the fence's twin, never the device's own object. When an encoder stops draining, fences need a barrier.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Between;
using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

namespace {

TEST(RawMetalFenceSource, EveryRawEncoderOpensWithADrain)
{
    const std::string gpu = SourceText("../raw/runtime/raw_gpu.cpp");
    EXPECT_THAT(Body(gpu, "BlitEncoder *CommandBuffer::blit()"), HasSubstr("e.p_.m(WAIT_FOR_IDLE, 0);"));
    EXPECT_THAT(Between(Body(gpu, "void RenderEncoder::end()"), "// pass prologue:", "for (uint32_t j = 0;"),
                HasSubstr("kit(p, switch_gm20b_pass_barrier, false);"));
    const std::string compute = SourceText("../raw/runtime/raw_compute.cpp");
    EXPECT_THAT(Body(compute, "ComputeEncoder::ComputeEncoder(CommandBuffer &cb, Device &dev)"),
                HasSubstr("p_.m(WAIT_FOR_IDLE, 0);"));
}

TEST(RawMetalFenceSource, TheRawEncodeEmitsNothingForAFence)
{
    const std::string encode = SourceText("../raw/runtime/RawMetalEncode.mm");
    EXPECT_THAT(encode, Not(HasSubstr("case OpUpdateFence")));
    EXPECT_THAT(encode, Not(HasSubstr("case OpWaitFence")));
}

TEST(RawMetalFenceSource, ReplayGetsTheFencesTwin)
{
    const std::string replay = SourceText("../raw/runtime/RawMetalReplay.mm");
    EXPECT_THAT(replay, HasSubstr("[enc updateFence:twinOf((__bridge id)c.obj)];"));
    EXPECT_THAT(replay, HasSubstr("[enc waitForFence:twinOf((__bridge id)c.obj)];"));
    const std::string fences = SourceText("../raw/runtime/RawMetalFences.mm");
    EXPECT_THAT(Body(fences, "- (id<MTLFence>)newFence"), Not(HasSubstr("fallback")));
}

}  // namespace
