// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <vector>
#include "../SwitchDisplayComposition.h"
#include "../SwitchDisplayMemoryEnvelope.h"
#include "../../SwitchPower/SwitchDisplayBandwidth.h"

namespace {
std::vector<uint64_t> reservations;
int reserveError;

int switch_emc_display_memory_reserve(const switch_display_memory_head *request)
{
    reservations.push_back(request->peak_bytes_per_second);
    return reserveError;
}

struct Display {
    Display()
    {
        switch_display_memory_head mode{};
        mode.pixel_clock_hz = 78000000;
        mode.active_width = 720;
        mode.active_height = 1280;
        mode.total_width = 1000;
        mode.total_height = 1300;
        EXPECT_TRUE(displayMemory.configure(mode, 720, 1280, false));
    }

    bool planeRecovering = false;
    bool entryPlaneSet[4] = {};
    using PlaneSet = switch_display::Composition;
    PlaneSet active;

    struct Latch {
        bool pending = false;
        int active = 0, armed = -1, superseded = -1;

        bool requested() const
        {
            return pending;
        }
    } planeLatch;

    bool planeHeldSet = false, gpuScanout = false, queued = false;

    void getDisplayDimensions(uint32_t *width, uint32_t *height)
    {
        *width = 720;
        *height = 1280;
    }

    const void *firstUntaken()
    {
        return queued ? this : nullptr;
    }

    const PlaneSet &planeSetOf(int)
    {
        return active;
    }

#include "../SwitchDisplayBandwidthDisplay.h"
};

native_planes::Layer image(uint32_t width, uint32_t height)
{
    native_planes::Layer layer;
    layer.kind = native_planes::kKindBgra;
    layer.surface.width = width;
    layer.surface.height = height;
    layer.surface.stride[0] = width * 4;
    layer.surface.allocation = layer.surface.descriptorBytes = uint64_t(width) * height * 4;
    layer.src = {0, 0, int32_t(width), int32_t(height)};
    layer.dst = {0, 0, 720, 1280};
    return layer;
}

class DisplayAdmission : public testing::Test {
protected:
    Display display;
    native_planes::Layer layers[4];
    bool present[4] = {true, true, false, false};

    void SetUp() override
    {
        reservations.clear();
        reserveError = 0;
        layers[0] = image(1440, 2560);
        layers[1] = image(720, 1280);
    }
};

TEST_F(DisplayAdmission, RaisesBeforeAcceptanceAndDoesNoExtraWorkAtArm)
{
    ASSERT_TRUE(display.validatePlaneBandwidth(present, layers, 720, 1280));
    ASSERT_EQ(reservations.size(), 1u);
    EXPECT_EQ(reservations[0], 1872000000u);
    Display::PlaneSet set;
    set.window[0].on = true;
    ASSERT_TRUE(native_planes::imageOf(layers[0], 720, 1280, set.window[0].image));
    EXPECT_TRUE(display.planeReserveBandwidth(set));
    EXPECT_EQ(reservations.size(), 1u);
}

TEST_F(DisplayAdmission, FirmwareRefusalDoesNotCommitAdmissionOrChangeVisibleFrame)
{
    display.active.window[0].on = true;
    reserveError = -5;
    EXPECT_FALSE(display.validatePlaneBandwidth(present, layers, 720, 1280));
    EXPECT_EQ(display.displayBandwidth.bytes(), 0u);
    EXPECT_TRUE(display.active.on());
}

TEST_F(DisplayAdmission, AllPendingConsumersPreserveOldReservationUntilRetirement)
{
    ASSERT_TRUE(display.validatePlaneBandwidth(present, layers, 720, 1280));
    display.planeLatch.pending = true;
    display.planeRetireBandwidth();
    display.planeLatch.pending = false;
    display.planeHeldSet = true;
    display.planeRetireBandwidth();
    display.planeHeldSet = false;
    display.gpuScanout = true;
    display.planeRetireBandwidth();
    display.gpuScanout = false;
    display.queued = true;
    display.planeRetireBandwidth();
    EXPECT_EQ(reservations.size(), 1u);
    display.queued = false;
    display.planeRetireBandwidth();
    ASSERT_EQ(reservations.size(), 2u);
    EXPECT_EQ(reservations[1], 312000000u);
}

TEST_F(DisplayAdmission, FailedLoweringKeepsHigherReservation)
{
    ASSERT_TRUE(display.validatePlaneBandwidth(present, layers, 720, 1280));
    reserveError = -5;
    display.planeRetireBandwidth();
    EXPECT_EQ(display.displayBandwidth.bytes(), 1872000000u);
}

TEST_F(DisplayAdmission, InheritedCompanionAndRgbConversionAreReservedBeforeAcceptance)
{
    layers[0] = image(720, 1280);
    layers[1] = image(1440, 2560);
    present[1] = false;
    ASSERT_TRUE(display.validatePlaneBandwidth(present, layers, 720, 1280));
    present[0] = false;
    present[1] = true;
    ASSERT_TRUE(display.validatePlaneBandwidth(present, layers, 720, 1280));
    const auto admitted = display.displayBandwidth.bytes();
    Display::PlaneSet set;
    set.window[0].on = set.window[1].on = true;
    ASSERT_TRUE(native_planes::imageOf(layers[0], 720, 1280, set.window[0].image));
    ASSERT_TRUE(native_planes::imageOf(layers[1], 720, 1280, set.window[1].image));
    EXPECT_TRUE(display.planeReserveBandwidth(set));
    EXPECT_EQ(display.displayBandwidth.bytes(), admitted);
    EXPECT_EQ(admitted, 1872000000u);
}

TEST_F(DisplayAdmission, CarriedUiCommitKeepsItsAcceptedEnvelope)
{
    ASSERT_TRUE(display.validatePlaneBandwidth(present, layers, 720, 1280));
    display.entryPlaneSet[2] = true;
    display.planeRetireBandwidth();
    EXPECT_EQ(reservations.size(), 1u);
    display.entryPlaneSet[2] = false;
    display.planeRetireBandwidth();
    EXPECT_EQ(reservations.size(), 2u);
}

}
