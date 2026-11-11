// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include "../raw/runtime/raw_fetch_order.h"

namespace {
using raw::FetchDraw;
using raw::FetchRefusal;

constexpr uint32_t kPoint = 0, kLine = 1, kLineStrip = 2, kTriangle = 3, kStrip = 4;

FetchRefusal refusal(bool fetchesOwn, bool caTransform, FetchDraw draw, bool primitiveId = false)
{
    return raw::fetchRefusal(fetchesOwn, caTransform, primitiveId, draw);
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

// The device's refused shapes (backboardd's list of six, SpringBoard's and iconservicesagent's strips of four, HW Test's
// list of 36): drawn a triangle at a time whatever their transform, no vertex read
TEST(FetchOrder, ListsAndStripsAreDrawnATriangleAtATimeWhateverTheirTransform)
{
    for (bool ca : {false, true}) {
        EXPECT_EQ(refusal(true, ca, {kTriangle, 6, 1}), FetchRefusal::None);
        EXPECT_EQ(refusal(true, ca, {kStrip, 4, 1}), FetchRefusal::None);
        EXPECT_EQ(refusal(true, ca, {kTriangle, 36, 1}), FetchRefusal::None);
        EXPECT_EQ(refusal(true, ca, {kTriangle, 6, 1, false, true}), FetchRefusal::None);
    }
}

TEST(FetchOrder, WithoutCoreAnimationsTransformOnlyWhatOneTriangleADrawCannotKeepIsRefused)
{
    EXPECT_EQ(refusal(true, false, {kStrip, 4, 1, false, true}), FetchRefusal::Restart);
    EXPECT_EQ(refusal(true, false, {kTriangle, 3 * raw::kSequentialTriangles, 1}), FetchRefusal::None);
    EXPECT_EQ(refusal(true, false, {kTriangle, 3 * raw::kSequentialTriangles + 3, 1}), FetchRefusal::Long);
    EXPECT_EQ(refusal(true, false, {kStrip, raw::kSequentialTriangles + 3, 1}), FetchRefusal::Long);
    EXPECT_EQ(refusal(true, false, {kTriangle, 6, 1}, true), FetchRefusal::PrimitiveId);
    EXPECT_EQ(refusal(true, false, {kTriangle, 3, 1}, true), FetchRefusal::None);
    // Core Animation's transform is left to submission, where its runs may be proven longer than one triangle
    EXPECT_EQ(refusal(true, true, {kStrip, 4, 1, false, true}), FetchRefusal::None);
    EXPECT_EQ(refusal(true, true, {kTriangle, 6, 1}, true), FetchRefusal::None);
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
    for (FetchRefusal r : {FetchRefusal::Topology, FetchRefusal::Instances, FetchRefusal::Restart, FetchRefusal::Long,
                           FetchRefusal::PrimitiveId, FetchRefusal::Geometry})
        EXPECT_STREQ(raw::fetchRefusalWhy(r),
                     "framebuffer-fetch ordering requires an interlocked program or available bounded geometry");
}

TEST(FetchOrder, EveryReasonHasALogName)
{
    for (size_t r = 0; r < raw::kFetchRefusals; r++)
        EXPECT_NE(raw::kFetchRefusalNames[r], nullptr);
    EXPECT_STREQ(raw::kFetchRefusalNames[size_t(FetchRefusal::Restart)], "restart");
    EXPECT_STREQ(raw::kFetchRefusalNames[size_t(FetchRefusal::Geometry)], "geometry");
}

}  // namespace
