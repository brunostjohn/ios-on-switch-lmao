// SPDX-License-Identifier: GPL-2.0-only
#include "../TegraDisplayB.h"
#include "drivers/SwitchUSB/test/xusb_register_model.h"
#include <gtest/gtest.h>

namespace {
constexpr uintptr_t dc = 0x54240000, kCar = 0x60006000, kPmc = 0x7000e400;
constexpr uintptr_t control = dc + 4 * 0x41, options = dc + 4 * 0x700, address = dc + 4 * 0x800;
constexpr uintptr_t kStateAccess = dc + 4 * 0x40, kDispWinOptions = dc + 4 * 0x402;
constexpr uintptr_t kDisplayCmd = dc + 4 * 0x32, kDisplayPwr = dc + 4 * 0x36;
constexpr uint32_t kMcHotResetCtrl = 0x200, kMcHotResetStatus = 0x204, kMcHotResetDcb = 1u << 3;
std::vector<switch_display_memory_head> plans;
bool admit;
uint32_t mcCtrl, mcStatusReads;
std::vector<bool> mcHotResetEvents;
}

extern "C" void tegra_mc_hot_reset(uint32_t ctrl, uint32_t bit, bool on)
{
    if (ctrl == kMcHotResetCtrl && bit == kMcHotResetDcb) {
        if (on)
            mcCtrl |= bit;
        else
            mcCtrl &= ~bit;
        mcHotResetEvents.push_back(on);
    }
}

extern "C" uint32_t tegra_smmu_read(uint32_t offset)
{
    if (offset == kMcHotResetCtrl)
        return mcCtrl;
    if (offset == kMcHotResetStatus) {
        ++mcStatusReads;
        return mcCtrl;
    }
    return 0;
}

extern "C" int switch_emc_display_memory_reserve(const switch_display_memory_head *head)
{
    plans.push_back(*head);
    return admit ? 0 : -1;
}

TEST(DisplayB, OnlyAnActivatedWindowCanRetireOrReleaseItsBandwidth)
{
    xusbModelReset();
    plans.clear();
    TegraDisplayB display;
    char why[96] = {};
    ASSERT_TRUE(display.powerUp(why, sizeof(why))) << why;
    switch_dp_mode mode = {};
    ASSERT_TRUE(switch_dp_cta_vic(16, &mode));
    display.setMode(mode);
    EXPECT_EQ(xusbModel().registers.at(dc + 4 * 0x405), 1u);
    EXPECT_EQ(xusbModel().registers.at(dc + 4 * 0x40), 1u);
    EXPECT_EQ(xusbModel().registers.at(dc + 4 * 0x43) & 4u, 0u);
    xusbModel().log.clear();
    admit = false;
    EXPECT_FALSE(display.scanout(0x10000000, 7680, 0));
    EXPECT_TRUE(xusbWritesTo(options).empty());
    EXPECT_TRUE(xusbWritesTo(address).empty());
    admit = true;
    ASSERT_TRUE(display.scanout(0x10000000, 7680, 0));
    ASSERT_EQ(plans.size(), 2u);
    EXPECT_EQ(plans.back().head, 1u);
    EXPECT_EQ(plans.back().peak_bytes_per_second, 594000000u);
    using Latch = TegraDisplayB::Latch;
    EXPECT_EQ(display.windowLatch(0x10000000), Latch::Pending);
    xusbModel().registers[control] = 0;
    EXPECT_EQ(display.windowLatch(0x20000000), Latch::Missed);
    EXPECT_EQ(display.windowLatch(0x10000000), Latch::Active);
    xusbModel().log.clear();
    ASSERT_TRUE(display.scanout(0x20000000, 7680, 0));
    xusbModel().registers[dc + 4 * 0x37] = 2;
    EXPECT_EQ(display.takeInterrupt(), 2u);
    EXPECT_EQ(display.windowLatch(0x20000000), Latch::Pending);
    display.frameInterrupt(true);
    EXPECT_TRUE(xusbWritesTo(dc + 4 * 0x39).empty());
    display.frameInterrupt(false);
    EXPECT_EQ(xusbWritesTo(dc + 4 * 0x39), std::vector<uint32_t>{0});
    EXPECT_EQ(xusbWritesTo(address), std::vector<uint32_t>{0x20000000});
    EXPECT_TRUE(xusbWritesTo(dc + 4 * 0x705).empty());
    EXPECT_TRUE(xusbWritesTo(dc + 4 * 0x70a).empty());
    EXPECT_EQ(plans.size(), 2u);
    display.windowOff();
    EXPECT_EQ(display.windowLatch(0), Latch::Pending);
    EXPECT_EQ(plans.size(), 2u);
    xusbModel().registers[control] = 0;
    xusbModel().registers[options] = 1u << 30;
    EXPECT_EQ(display.windowLatch(0), Latch::Missed);
    EXPECT_EQ(plans.size(), 2u);
    xusbModel().registers[options] = 0;
    EXPECT_EQ(display.windowLatch(0), Latch::Active);
    ASSERT_EQ(plans.size(), 3u);
    EXPECT_EQ(plans.back().head, 1u);
    EXPECT_EQ(plans.back().peak_bytes_per_second, 0u);
}

TEST(DisplayB, TeardownUsesActiveWriteMuxAndFlushesMcHotResetBeforeReset)
{
    xusbModelReset();
    plans.clear();
    mcCtrl = kMcHotResetDcb;
    mcStatusReads = 0;
    mcHotResetEvents.clear();
    auto &model = xusbModel();
    model.afterWrite = [](uintptr_t addr, uint32_t value) {
        auto &r = xusbModel().registers;
        if (addr == kCar + 0x320)
            r[kCar + 0x010] |= value;
        else if (addr == kCar + 0x324)
            r[kCar + 0x010] &= ~value;
        else if (addr == kCar + 0x300)
            r[kCar + 0x004] |= value;
        else if (addr == kCar + 0x304)
            r[kCar + 0x004] &= ~value;
    };
    TegraDisplayB display;
    char why[96] = {};
    ASSERT_TRUE(display.powerUp(why, sizeof(why))) << why;
    EXPECT_EQ(mcCtrl, 0u);
    switch_dp_mode mode = {};
    ASSERT_TRUE(switch_dp_cta_vic(16, &mode));
    display.setMode(mode);

    model.log.clear();
    display.enableSor(true);
    EXPECT_EQ(xusbWritesTo(kStateAccess), (std::vector<uint32_t>{5u, 1u}));
    EXPECT_EQ(xusbWritesTo(kDispWinOptions), (std::vector<uint32_t>{1u << 26}));
    EXPECT_LT(xusbFirstWrite(kStateAccess), xusbFirstWrite(kDispWinOptions));
    EXPECT_EQ(model.registers[kDispWinOptions], 1u << 26);

    model.log.clear();
    display.enableSor(false);
    EXPECT_EQ(xusbWritesTo(kStateAccess), (std::vector<uint32_t>{5u, 1u}));
    EXPECT_EQ(xusbWritesTo(kDispWinOptions), (std::vector<uint32_t>{0u}));
    const long activeMux = xusbFirstWrite(kStateAccess);
    const long clearSor = xusbFirstWrite(kDispWinOptions);
    EXPECT_LT(activeMux, clearSor);

    model.log.clear();
    mcStatusReads = 0;
    mcHotResetEvents.clear();
    display.powerDown();
    EXPECT_EQ(xusbWritesTo(kStateAccess), (std::vector<uint32_t>{5u, 1u}));
    EXPECT_EQ(xusbWritesTo(kDispWinOptions), (std::vector<uint32_t>{0u}));
    EXPECT_EQ(xusbWritesTo(kDisplayCmd), (std::vector<uint32_t>{0u}));
    EXPECT_EQ(xusbWritesTo(kDisplayPwr), (std::vector<uint32_t>{0u}));
    EXPECT_EQ(mcHotResetEvents, (std::vector<bool>{true}));
    EXPECT_EQ(mcCtrl, kMcHotResetDcb);
    EXPECT_GE(mcStatusReads, 5u);
    const long resetAssert = xusbFirstWrite(kCar + 0x300, 1u << 26);
    const long clockDisable = xusbFirstWrite(kCar + 0x324, 1u << 26);
    const long pmcOff = xusbFirstWrite(kPmc + 0x030);
    ASSERT_GE(resetAssert, 0);
    ASSERT_GE(clockDisable, 0);
    ASSERT_GE(pmcOff, 0);
    EXPECT_LT(resetAssert, clockDisable);
    EXPECT_LT(clockDisable, pmcOff);
    EXPECT_EQ(model.registers[kPmc + 0x038] & (1u << 19), 0u);
}
