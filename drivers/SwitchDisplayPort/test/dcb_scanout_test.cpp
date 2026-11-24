// SPDX-License-Identifier: GPL-2.0-only
#include "../TegraDisplayB.h"
#include "../TegraDpaux1.h"
#include "../TegraSor1.h"
#include "drivers/SwitchUSB/test/xusb_register_model.h"
#include <gtest/gtest.h>

namespace {
constexpr uintptr_t dc = 0x54240000, kSor = 0x54580000, kDpaux = 0x54040000;
constexpr uintptr_t kCar = 0x60006000, kPmc = 0x7000e400;
constexpr uintptr_t control = dc + 4 * 0x41, header = dc + 4 * 0x42, options = dc + 4 * 0x700,
                    address = dc + 4 * 0x800;
constexpr uintptr_t kStateAccess = dc + 4 * 0x40, kDispWinOptions = dc + 4 * 0x402;
constexpr uintptr_t kDisplayCmd = dc + 4 * 0x32, kDisplayPwr = dc + 4 * 0x36;
constexpr uintptr_t kWinBufCtrl = dc + 4 * 0x702, kWinColorDepth = dc + 4 * 0x703, kWinPos = dc + 4 * 0x704;
constexpr uintptr_t kWinSize = dc + 4 * 0x705, kWinPrescaled = dc + 4 * 0x706;
constexpr uintptr_t kWinLineStride = dc + 4 * 0x70a, kWinBufStride = dc + 4 * 0x70b, kWinUvBufStride = dc + 4 * 0x70c,
                    kWinBlend = dc + 4 * 0x716;
constexpr uintptr_t kWinHOffset = dc + 4 * 0x806, kWinVOffset = dc + 4 * 0x808, kWinKind = dc + 4 * 0x80b,
                    kWinStartAddrHi = dc + 4 * 0x80d, kWinCdeCtrl = dc + 4 * 0x82f;
constexpr uint32_t kMcHotResetCtrl = 0x200, kMcHotResetStatus = 0x204, kMcHotResetDcb = 1u << 3;
std::vector<switch_display_memory_head> plans;
bool admit;
uint32_t mcCtrl, mcStatusReads;
std::vector<bool> mcHotResetEvents;

TegraDisplayB::Frame fullFrame(uint32_t iova, uint32_t stride = 7680, uint32_t kind = 0)
{
    return {iova, stride, kind, 12u, 0u, (1080u << 16) | 1920u, (1080u << 16) | (1920u * 4u), 0u, 0u};
}
}

extern "C" int tegra_i2c_write(unsigned, uint8_t, uint8_t, const void *, size_t)
{
    return 0;
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
    xusbModel().log.clear();
    display.setMode(mode);
    EXPECT_EQ(xusbWritesTo(control), (std::vector<uint32_t>{0x300u, 0x3u}));
    EXPECT_EQ(xusbModel().registers.at(dc + 4 * 0x405), 1u);
    EXPECT_EQ(xusbModel().registers.at(dc + 4 * 0x40), 1u);
    EXPECT_EQ(xusbModel().registers.at(dc + 4 * 0x43) & 4u, 0u);
    EXPECT_EQ(xusbModel().registers.at(kWinBufCtrl), 0u);
    EXPECT_EQ(xusbModel().registers.at(kWinBufStride), 0u);
    EXPECT_EQ(xusbModel().registers.at(kWinUvBufStride), 0u);
    EXPECT_EQ(xusbModel().registers.at(kWinStartAddrHi), 0u);
    EXPECT_EQ(xusbModel().registers.at(kWinCdeCtrl), 0u);
    xusbModel().log.clear();
    admit = false;
    ASSERT_TRUE(display.scanout(fullFrame(0x10000000)));
    EXPECT_EQ(xusbWritesTo(options), std::vector<uint32_t>{1u << 30});
    EXPECT_EQ(xusbWritesTo(address), std::vector<uint32_t>{0x10000000});
    ASSERT_EQ(plans.size(), 1u);
    EXPECT_EQ(plans.back().windows[0].required_buffering_bytes, 0u);
    display.windowOff();
    xusbModel().registers[control] = 0;
    xusbModel().registers[options] = 0;
    EXPECT_EQ(display.windowLatch(0), TegraDisplayB::Latch::Active);
    EXPECT_EQ(plans.size(), 1u);

    xusbModel().log.clear();
    admit = true;
    ASSERT_TRUE(display.scanout(fullFrame(0x10000000, 7680, 0x42u)));
    ASSERT_EQ(plans.size(), 2u);
    EXPECT_EQ(plans.back().head, 1u);
    EXPECT_EQ(plans.back().peak_bytes_per_second, 594000000u);
    EXPECT_GT(plans.back().windows[0].required_buffering_bytes, 0u);
    using Latch = TegraDisplayB::Latch;
    EXPECT_EQ(display.windowLatch(0x10000000), Latch::Pending);
    xusbModel().registers[control] = 0;
    EXPECT_EQ(display.windowLatch(0x20000000), Latch::Missed);
    EXPECT_EQ(display.windowLatch(0x10000000), Latch::Active);
    xusbModel().log.clear();
    ASSERT_TRUE(display.scanout(fullFrame(0x20000000, 7680, 0x42u)));
    xusbModel().registers[dc + 4 * 0x37] = 2;
    const unsigned statusReadsBefore = xusbModel().reads[dc + 4 * 0x37];
    EXPECT_EQ(display.takeInterrupt(), 2u);
    EXPECT_EQ(xusbModel().reads[dc + 4 * 0x37], statusReadsBefore + 2u);
    EXPECT_EQ(display.windowLatch(0x20000000), Latch::Pending);
    display.frameInterrupt(true);
    EXPECT_TRUE(xusbWritesTo(dc + 4 * 0x39).empty());
    display.frameInterrupt(false);
    EXPECT_EQ(xusbWritesTo(dc + 4 * 0x39), std::vector<uint32_t>{0});
    EXPECT_EQ(xusbWritesTo(address), std::vector<uint32_t>{0x20000000});
    EXPECT_EQ(xusbWritesTo(header), std::vector<uint32_t>{1u << 4});
    EXPECT_LT(xusbFirstWrite(header), xusbFirstWrite(address));
    EXPECT_TRUE(xusbWritesTo(kWinSize).empty());
    EXPECT_TRUE(xusbWritesTo(kWinLineStride).empty());
    EXPECT_EQ(plans.size(), 2u);

    xusbModel().log.clear();
    display.resetWindowCache();
    ASSERT_TRUE(display.scanout(fullFrame(0x20000000, 7680, 0x42u)));
    EXPECT_EQ(xusbWritesTo(options), std::vector<uint32_t>{1u << 30});
    EXPECT_EQ(xusbWritesTo(kWinSize), std::vector<uint32_t>{(1080u << 16) | 1920u});
    EXPECT_EQ(xusbWritesTo(kWinLineStride), std::vector<uint32_t>{7680u});
    EXPECT_EQ(plans.size(), 3u);
    xusbModel().log.clear();
    display.windowOff();
    EXPECT_EQ(xusbWritesTo(header), std::vector<uint32_t>{1u << 4});
    EXPECT_LT(xusbFirstWrite(header), xusbFirstWrite(options));
    EXPECT_EQ(display.windowLatch(0), Latch::Pending);
    EXPECT_EQ(plans.size(), 3u);
    xusbModel().registers[control] = 0;
    xusbModel().registers[options] = 1u << 30;
    EXPECT_EQ(display.windowLatch(0), Latch::Missed);
    EXPECT_EQ(plans.size(), 3u);
    xusbModel().registers[options] = 0;
    EXPECT_EQ(display.windowLatch(0), Latch::Active);
    ASSERT_EQ(plans.size(), 4u);
    EXPECT_EQ(plans.back().head, 1u);
    EXPECT_EQ(plans.back().peak_bytes_per_second, 0u);
}

TEST(DisplayB, ProgramsWindowGeometryForPillarboxedAndSubRectFrames)
{
    xusbModelReset();
    plans.clear();
    admit = true;
    TegraDisplayB display;
    char why[96] = {};
    ASSERT_TRUE(display.powerUp(why, sizeof(why))) << why;
    switch_dp_mode mode = {};
    ASSERT_TRUE(switch_dp_cta_vic(16, &mode));
    display.setMode(mode);

    xusbModel().log.clear();
    const TegraDisplayB::Frame pillar = {
        0x10000000u, 2432u, 0u, 12u, 656u, (1080u << 16) | 608u, (1080u << 16) | (608u * 4u), 0u, 0u,
    };
    ASSERT_TRUE(display.scanout(pillar));
    EXPECT_EQ(xusbWritesTo(kWinBlend), std::vector<uint32_t>{(1u << 24) | 255u});
    EXPECT_EQ(xusbWritesTo(kWinColorDepth), std::vector<uint32_t>{12u});
    EXPECT_EQ(xusbWritesTo(kWinPos), std::vector<uint32_t>{656u});
    EXPECT_EQ(xusbWritesTo(kWinSize), std::vector<uint32_t>{(1080u << 16) | 608u});
    EXPECT_EQ(xusbWritesTo(kWinPrescaled), std::vector<uint32_t>{(1080u << 16) | (608u * 4u)});
    EXPECT_EQ(xusbWritesTo(kWinLineStride), std::vector<uint32_t>{2432u});
    EXPECT_EQ(xusbWritesTo(kWinHOffset), std::vector<uint32_t>{0u});
    EXPECT_EQ(xusbWritesTo(kWinVOffset), std::vector<uint32_t>{0u});
    EXPECT_EQ(xusbWritesTo(kWinKind), std::vector<uint32_t>{0u});
    EXPECT_EQ(xusbWritesTo(address), std::vector<uint32_t>{0x10000000u});
    EXPECT_EQ(plans.back().peak_bytes_per_second, 594000000u);

    xusbModel().registers[control] = 0;
    EXPECT_EQ(display.windowLatch(0x10000000u), TegraDisplayB::Latch::Active);

    xusbModel().log.clear();
    const TegraDisplayB::Frame subRect = {
        0x1004c000u, 2560u, 0x42u, 13u, (40u << 16) | 656u, (1000u << 16) | 608u, (1000u << 16) | (608u * 4u),
        64u, 22u,
    };
    ASSERT_TRUE(display.scanout(subRect));
    EXPECT_EQ(xusbWritesTo(kWinColorDepth), std::vector<uint32_t>{13u});
    EXPECT_EQ(xusbWritesTo(kWinPos), std::vector<uint32_t>{(40u << 16) | 656u});
    EXPECT_EQ(xusbWritesTo(kWinSize), std::vector<uint32_t>{(1000u << 16) | 608u});
    EXPECT_EQ(xusbWritesTo(kWinPrescaled), std::vector<uint32_t>{(1000u << 16) | (608u * 4u)});
    EXPECT_EQ(xusbWritesTo(kWinLineStride), std::vector<uint32_t>{2560u});
    EXPECT_EQ(xusbWritesTo(kWinHOffset), std::vector<uint32_t>{64u});
    EXPECT_EQ(xusbWritesTo(kWinVOffset), std::vector<uint32_t>{22u});
    EXPECT_EQ(xusbWritesTo(kWinKind), std::vector<uint32_t>{0x42u});
    EXPECT_EQ(xusbWritesTo(address), std::vector<uint32_t>{0x1004c000u});
    EXPECT_EQ(plans.size(), 1u);

    xusbModel().registers[control] = 0;
    EXPECT_EQ(display.windowLatch(0x1004c000u), TegraDisplayB::Latch::Active);
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
    EXPECT_EQ(xusbWritesTo(kStateAccess), (std::vector<uint32_t>{1u, 5u, 1u}));
    EXPECT_EQ(xusbWritesTo(kDispWinOptions), (std::vector<uint32_t>{1u << 26, 1u << 26}));
    EXPECT_LT(xusbFirstWrite(kStateAccess), xusbFirstWrite(kDispWinOptions));
    EXPECT_EQ(model.registers[kDispWinOptions], 1u << 26);

    model.log.clear();
    display.enableSor(false);
    EXPECT_EQ(xusbWritesTo(kStateAccess), (std::vector<uint32_t>{1u, 5u, 1u}));
    EXPECT_EQ(xusbWritesTo(kDispWinOptions), (std::vector<uint32_t>{0u, 0u}));
    const long activeMux = xusbFirstWrite(kStateAccess, 5u);
    const long clearSor = xusbFirstWrite(kDispWinOptions);
    EXPECT_GT(activeMux, clearSor);

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

TEST(DisplayB, CombinedTeardownKeepsPllD2RunningThroughDcbPowerDown)
{
    xusbModelReset();
    plans.clear();
    admit = true;
    mcCtrl = 0;
    mcStatusReads = 0;
    mcHotResetEvents.clear();

    constexpr uintptr_t kPllD2 = kCar + 0x4b8, kPllDp = kCar + 0x590;
    constexpr uintptr_t kSuperState0 = kSor + 0x004, kSuperState1 = kSor + 0x008;
    constexpr uintptr_t kHeadState0 = kSor + 0x018, kHeadState1 = kSor + 0x020;
    constexpr uintptr_t kSorPwr = kSor + 0x054, kSorTest = kSor + 0x058, kLaneSeq = kSor + 0x084;

    auto &model = xusbModel();
    auto &r = model.registers;
    r[0x70000804] = (0x21u << 8) | (2u << 4);
    r[kPmc + 0x038] = 1u << 17;

    unsigned testReadsBeforeDetach = 0;
    bool sawDetachClear = false;
    bool dcbWriteAfterPllD2Off = false;

    model.afterWrite = [&](uintptr_t addr, uint32_t value) {
        auto &reg = xusbModel().registers;
        if (addr == kPllD2 || addr == kPllDp) {
            if (value & (1u << 30))
                reg[addr] |= 1u << 27;
            else
                reg[addr] &= ~(1u << 27);
        }
        if (addr >= dc && addr < dc + 0x40000 && (reg[kPllD2] & (1u << 30)) == 0u)
            dcbWriteAfterPllD2Off = true;
        if (addr == kSuperState1 && !sawDetachClear && (value & (1u << 3)) == 0u &&
            (reg[kSorTest] & (1u << 10)) != 0u) {
            sawDetachClear = true;
            testReadsBeforeDetach = xusbModel().reads[kSorTest];
        }
        if (addr == kSuperState0 && value == 1u) {
            const uint32_t ss1 = reg[kSuperState1];
            uint32_t test = 0;
            if (ss1 & (1u << 3))
                test |= 1u << 10;
            test |= (ss1 & 3u) << 8;
            reg[kSorTest] = test;
            if ((ss1 & (1u << 2)) == 0u)
                reg[kSorPwr] |= 1u << 28;
            else
                reg[kSorPwr] &= ~(1u << 28);
        }
        if (addr == kLaneSeq || addr == kSorPwr)
            reg[addr] &= ~(1u << 31);
        if (addr == kCar + 0x320)
            reg[kCar + 0x010] |= value;
        else if (addr == kCar + 0x324)
            reg[kCar + 0x010] &= ~value;
        else if (addr == kCar + 0x300)
            reg[kCar + 0x004] |= value;
        else if (addr == kCar + 0x304)
            reg[kCar + 0x004] &= ~value;
        else if (addr == kCar + 0x284)
            reg[kCar + 0x280] |= value;
        else if (addr == kCar + 0x288)
            reg[kCar + 0x280] &= ~value;
        else if (addr == kCar + 0x29c)
            reg[kCar + 0x298] |= value;
        else if (addr == kCar + 0x2a0)
            reg[kCar + 0x298] &= ~value;
        else if (addr == kCar + 0x2a8)
            reg[kCar + 0x2a4] |= value;
        else if (addr == kCar + 0x2ac)
            reg[kCar + 0x2a4] &= ~value;
        else if (addr == kPmc + 0x1c0) {
            if ((value >> 30) == 1)
                reg[kPmc + 0x1c4] &= ~(value & 0x3fffffffu);
            else if ((value >> 30) == 2)
                reg[kPmc + 0x1c4] |= value & 0x3fffffffu;
        }
    };

    TegraDpaux1 aux;
    TegraSor1 sor;
    TegraDisplayB dcb;
    char why[160] = {};
    ASSERT_TRUE(aux.powerUp(why, sizeof(why))) << why;
    switch_dp_mode mode = {};
    ASSERT_TRUE(switch_dp_cta_vic(16, &mode));
    const switch_dp_link link = {SWITCH_DP_RATE_HBR, 2};
    switch_dp_tu tu = {};
    ASSERT_TRUE(switch_dp_tu_config(mode.pixel_khz, mode.hactive, switch_dp_mode_htotal(&mode),
                                    link.rate, link.lanes, true, &tu));
    ASSERT_TRUE(sor.powerUp(why, sizeof(why))) << why;
    ASSERT_TRUE(sor.pixelClock(mode.pixel_khz, why, sizeof(why))) << why;
    ASSERT_TRUE(dcb.powerUp(why, sizeof(why))) << why;
    dcb.setMode(mode);
    ASSERT_TRUE(sor.startLink(link, true, why, sizeof(why))) << why;
    sor.enableLink(tu);

    model.log.clear();
    ASSERT_TRUE(sor.attach(mode, tu, why, sizeof(why))) << why;
    const long headState0Write = xusbFirstWrite(kHeadState0);
    const long headState1Write = xusbFirstWrite(kHeadState1);
    ASSERT_GE(headState0Write, 0);
    ASSERT_GE(headState1Write, 0);
    EXPECT_LT(headState0Write, headState1Write);
    EXPECT_EQ(r[kHeadState0], 0u);

    dcb.enableSor(true);
    ASSERT_TRUE(sor.wake(why, sizeof(why))) << why;

    model.log.clear();
    const unsigned testReadsAtStart = model.reads[kSorTest];
    dcbWriteAfterPllD2Off = false;

    dcb.windowOff();
    sor.detachHead();
    dcb.enableSor(false);
    dcb.powerDown();
    EXPECT_NE(r[kPllD2] & (1u << 30), 0u);
    sor.powerDown();

    EXPECT_FALSE(dcbWriteAfterPllD2Off);
    EXPECT_TRUE(sawDetachClear);
    EXPECT_GT(testReadsBeforeDetach, testReadsAtStart);
    EXPECT_EQ(r[kPllD2] & (1u << 30), 0u);

    const long disp2Reset = xusbFirstWrite(kCar + 0x300, 1u << 26);
    const long disp2ClkOff = xusbFirstWrite(kCar + 0x324, 1u << 26);
    long pllD2Off = -1;
    for (size_t i = 0; i < model.log.size(); ++i) {
        if (model.log[i].kind == XusbEvent::Write && model.log[i].address == kPllD2 &&
            (model.log[i].value & (1u << 30)) == 0u) {
            pllD2Off = static_cast<long>(i);
            break;
        }
    }
    ASSERT_GE(disp2Reset, 0);
    ASSERT_GE(disp2ClkOff, 0);
    ASSERT_GE(pllD2Off, 0);
    EXPECT_LT(disp2Reset, pllD2Off);
    EXPECT_LT(disp2ClkOff, pllD2Off);

    aux.powerDown();
    model.log.clear();
    aux.powerDown();
    EXPECT_TRUE(model.log.empty());
    EXPECT_EQ(xusbWritesTo(kDpaux + 0x138).size(), 0u);
}
