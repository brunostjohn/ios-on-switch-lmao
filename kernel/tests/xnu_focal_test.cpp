// kernel/switch/include/switch/focal.h on the host: the places the debug watchers keep for focal apps.
#include <gtest/gtest.h>

#include "switch/focal.h"

namespace {

constexpr uint64_t kHWTest = 0x1000, kScreenshots = 0x2000, kPreferences = 0x3000, kMaps = 0x4000;

// 25 Nov: a Capture press made ScreenshotServicesService focal over HW Test; when it left, HW Test lost its watch for
// 15 minutes, although it was focal all along.
TEST(XnuFocal, AnOverlayLeavingKeepsTheAppUnderItWatched)
{
    switch_focal f{};
    uint64_t evicted = 1;

    const unsigned hwtest = switch_focal_enter(&f, kHWTest, 10, &evicted);
    EXPECT_EQ(evicted, 0u);
    const unsigned overlay = switch_focal_enter(&f, kScreenshots, 20, &evicted);
    EXPECT_NE(overlay, hwtest);
    EXPECT_EQ(switch_focal_leave(&f, kScreenshots), overlay);
    EXPECT_EQ(f.owner[hwtest], kHWTest) << "HW Test keeps its place";
    EXPECT_EQ(switch_focal_leave(&f, kScreenshots), SWITCH_FOCAL_PLACES) << "a second leave frees nothing";
    EXPECT_EQ(switch_focal_leave(&f, kHWTest), hwtest);
    for (unsigned i = 0; i < SWITCH_FOCAL_PLACES; i++) {
        EXPECT_EQ(f.owner[i], 0u);
    }
}

TEST(XnuFocal, ATaskFocalAgainKeepsItsPlace)
{
    switch_focal f{};
    uint64_t evicted = 0;

    const unsigned first = switch_focal_enter(&f, kHWTest, 10, &evicted);
    EXPECT_EQ(switch_focal_enter(&f, kHWTest, 50, &evicted), first);
    EXPECT_EQ(evicted, 0u);
}

TEST(XnuFocal, AFourthFocalAppTakesThePlaceFocalTheLongest)
{
    switch_focal f{};
    uint64_t evicted = 0;

    switch_focal_enter(&f, kHWTest, 30, &evicted);
    const unsigned oldest = switch_focal_enter(&f, kScreenshots, 10, &evicted);
    switch_focal_enter(&f, kPreferences, 20, &evicted);
    EXPECT_EQ(switch_focal_enter(&f, kMaps, 40, &evicted), oldest);
    EXPECT_EQ(evicted, kScreenshots);
    EXPECT_EQ(switch_focal_leave(&f, kScreenshots), SWITCH_FOCAL_PLACES);
}

}  // namespace
