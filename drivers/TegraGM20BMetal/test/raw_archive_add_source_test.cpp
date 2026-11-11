// SPDX-License-Identifier: MIT
// Guards on the facade's binary archives, which are m2v's (RawMetalArchive.mm) and which no host model runs: Metal adds a
// device's own functions to its archives, the facade's descriptors name its functions, and m2v's archive takes only m2v's
// (NativeBridgeMetalFrontend.mm renderDescriptor and computeDescriptor refuse a foreign function).
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;

namespace {

TEST(RawArchiveAddSource, M2VsArchiveTakesOnlyItsOwnFunctions)
{
    const std::string frontend = SourceText("../../NativeMetal/NativeBridgeMetalFrontend.mm");
    EXPECT_THAT(Body(frontend, "static bool renderDescriptor(MTLRenderPipelineDescriptor *d,"),
                HasSubstr("isKindOfClass:[NBFunction class]"));
    EXPECT_THAT(Body(frontend, "static bool computeDescriptor(MTLComputePipelineDescriptor *d,"),
                HasSubstr("isKindOfClass:[NBFunction class]"));
}

TEST(RawArchiveAddSource, TheFacadesArchiveAddsTheTwinsOfItsDescriptorsFunctions)
{
    const std::string archive = SourceText("../raw/runtime/RawMetalArchive.mm");
    EXPECT_THAT(Body(archive, "- (id<MTLBinaryArchive>)newBinaryArchiveWithDescriptor:(MTLBinaryArchiveDescriptor *)d"),
                HasSubstr("object_setClass(archive, facadeArchive("));
    const std::string facade = Body(archive, "static Class facadeArchive(Class m2v)");
    EXPECT_THAT(facade, HasSubstr("@selector(addRenderPipelineFunctionsWithDescriptor:error:)"));
    EXPECT_THAT(facade, HasSubstr("@selector(addComputePipelineFunctionsWithDescriptor:error:)"));
    EXPECT_THAT(Body(archive, "static BOOL addRenderTwins("), HasSubstr("twinDescriptor(d)"));
    EXPECT_THAT(Body(archive, "static BOOL addComputeTwins("), HasSubstr("twinOf(d.computeFunction)"));
}

}  // namespace
