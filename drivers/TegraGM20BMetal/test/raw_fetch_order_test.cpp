// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include "../raw/runtime/raw_fetch_order.h"

namespace {
using raw::FetchDraw;
using raw::FetchRefusal;

constexpr uint32_t kPoint = 0, kLine = 1, kLineStrip = 2, kTriangle = 3, kStrip = 4;

FetchRefusal refusal(bool fetchesOwn, bool caTransform, FetchDraw draw)
{
    return raw::fetchRefusal(fetchesOwn, caTransform, draw);
}

TEST(FetchOrder, OnlyAColorFetchedAndWrittenOutsideTheInterlockCounts)
{
    EXPECT_TRUE(raw::fetchesOwnColor(1, 1, 0));
    EXPECT_TRUE(raw::fetchesOwnColor(3, 2, 1));
    EXPECT_FALSE(raw::fetchesOwnColor(1, 2, 0));
    EXPECT_FALSE(raw::fetchesOwnColor(0, 1, 0));
    EXPECT_FALSE(raw::fetchesOwnColor(1, 1, 1));
}

TEST(FetchOrder, ADrawThatFetchesNoColorItWritesIsNeverRefused)
{
    for (bool ca : {false, true})
        for (uint32_t primitive : {kPoint, kLine, kLineStrip, kTriangle, kStrip})
            for (bool indirect : {false, true})
                EXPECT_EQ(refusal(false, ca, {primitive, 600, 7, indirect}), FetchRefusal::None);
}

TEST(FetchOrder, OnePrimitiveIsDrawnWholeWhateverItsTransform)
{
    EXPECT_EQ(refusal(true, false, {kTriangle, 3, 1}), FetchRefusal::None);
    EXPECT_EQ(refusal(true, false, {kStrip, 3, 1}), FetchRefusal::None);
    EXPECT_EQ(refusal(true, false, {kPoint, 1, 1}), FetchRefusal::None);
    EXPECT_EQ(refusal(true, false, {kLine, 2, 1}), FetchRefusal::None);
    EXPECT_EQ(refusal(true, false, {kLineStrip, 2, 1}), FetchRefusal::None);
    EXPECT_EQ(refusal(true, false, {kTriangle, 0, 1}), FetchRefusal::None);
    EXPECT_EQ(refusal(true, false, {kTriangle, 6, 0}), FetchRefusal::None);
}

// The device's refused shape: a list of six, one instance. Core Animation's transform leaves it to submission, where the
// CPU reads its vertices if it can; anything else has nothing the CPU can bound.
TEST(FetchOrder, AQuadIsRefusedAsRecordedOnlyWithoutCoreAnimationsTransform)
{
    EXPECT_EQ(refusal(true, false, {kTriangle, 6, 1}), FetchRefusal::Transform);
    EXPECT_EQ(refusal(true, true, {kTriangle, 6, 1}), FetchRefusal::None);
    EXPECT_EQ(refusal(true, false, {kStrip, 4, 1}), FetchRefusal::Transform);
    EXPECT_EQ(refusal(true, true, {kStrip, 4, 1}), FetchRefusal::None);
}

TEST(FetchOrder, InstancesTopologyAndIndirectCountsAreRefusedAsRecorded)
{
    for (bool ca : {false, true}) {
        EXPECT_EQ(refusal(true, ca, {kTriangle, 6, 2}), FetchRefusal::Instances);
        EXPECT_EQ(refusal(true, ca, {kTriangle, 3, 2}), FetchRefusal::Instances);
        EXPECT_EQ(refusal(true, ca, {kPoint, 2, 1}), FetchRefusal::Topology);
        EXPECT_EQ(refusal(true, ca, {kLine, 4, 1}), FetchRefusal::Topology);
        EXPECT_EQ(refusal(true, ca, {kLineStrip, 3, 1}), FetchRefusal::Topology);
        EXPECT_EQ(refusal(true, ca, {kTriangle, 0, 1, true}), FetchRefusal::Indirect);
        EXPECT_EQ(refusal(true, ca, {kPoint, 0, 1, true}), FetchRefusal::Indirect);
    }
}

TEST(FetchOrder, TheReasonsKeepTheEncodeLinesWording)
{
    EXPECT_STREQ(raw::fetchRefusalWhy(FetchRefusal::Indirect),
                 "indirect framebuffer-fetch draws require an interlocked program");
    for (FetchRefusal r :
         {FetchRefusal::Topology, FetchRefusal::Instances, FetchRefusal::Transform, FetchRefusal::Geometry})
        EXPECT_STREQ(raw::fetchRefusalWhy(r),
                     "framebuffer-fetch ordering requires an interlocked program or available bounded geometry");
}

}  // namespace
