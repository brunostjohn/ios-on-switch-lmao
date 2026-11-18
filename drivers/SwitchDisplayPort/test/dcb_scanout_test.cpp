// SPDX-License-Identifier: GPL-2.0-only
#include "../TegraDisplayB.h"
#include "drivers/SwitchUSB/test/xusb_register_model.h"
#include <gtest/gtest.h>

namespace {
constexpr uintptr_t dc = 0x54240000;
constexpr uintptr_t control = dc + 4 * 0x41, options = dc + 4 * 0x700, address = dc + 4 * 0x800;
std::vector<switch_display_memory_head> plans;
bool admit;
}

extern "C" void tegra_mc_hot_reset(uint32_t, uint32_t, bool) {}
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
