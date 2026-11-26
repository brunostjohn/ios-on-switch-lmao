// SPDX-License-Identifier: BSD-2-Clause-Patent
// host1x's general interrupt (SwitchHost1xErrors.cpp) on a sync-unit register model whose HINTSTATUS_EXT clears where
// a one is written (TRM 14.9.7): the attach and its masks, a stopped channel taken once and masked until its restart,
// its jobs failed by the action, and a debug kernel's expected error (the illegal-opcode probe) delivered or lost.
#include "switch_host1x_fence_stubs.h"
#include <gtest/gtest.h>
#include <map>
#include <string>
#include <vector>
#include "../SwitchHost1xErrors.cpp"

namespace {
constexpr uintptr_t kSync = 0x50002100;
std::map<uint32_t, uint32_t> gRegs;
bool gIntmaskStuck, gNoNub;
std::vector<uint32_t> gFailedChannels;

uint32_t &reg(uint32_t offset)
{
    return gRegs[offset];
}

bool anyLine(const std::string &text)
{
    for (const auto &line : switch_test_iolog)
        if (line.find(text) != std::string::npos)
            return true;
    return false;
}

IOFilterInterruptEventSource *source()
{
    return IOFilterInterruptEventSource::made().empty() ? nullptr : IOFilterInterruptEventSource::made().back();
}

bool fire(uint32_t channel)
{
    reg(kHintStatusExt) |= 1u << channel;
    if (!source()->filter(nullptr, source()))
        return false;
    source()->action(nullptr, source(), 1);
    return true;
}

struct Host1xErrors : testing::Test {
    void SetUp() override
    {
        gRegs = {{kIntMask, 0x10}};
        gIntmaskStuck = gNoNub = false;
        gFailedChannels.clear();
        switch_test_iolog.clear();
        switch_test_sleep = nullptr;
        gErrorsSource = nullptr;
    }

    bool start()
    {
        return switch_host1x_errors_start(IOWorkLoop::workLoop(), reinterpret_cast<volatile uint8_t *>(kSync));
    }
};
}  // namespace

extern "C" {
uint32_t switch_mmio_model_read32(uintptr_t address)
{
    return gRegs[static_cast<uint32_t>(address - kSync)];
}

void switch_mmio_model_write32(uintptr_t address, uint32_t value)
{
    const auto offset = static_cast<uint32_t>(address - kSync);
    if (offset == kHintStatusExt)
        gRegs[offset] &= ~value;
    else if (offset != kIntMask || !gIntmaskStuck)
        gRegs[offset] = value;
}

void tegra_host1x_describe_channel(uint32_t channel, char *buffer, size_t size)
{
    snprintf(buffer, size, "ch%u DMAGET 0x40", channel);
}
}

IOService *switch_interrupt_nub(uint32_t, bool)
{
    static IOService nub;
    return gNoNub ? nullptr : &nub;
}

void switch_host1x_channels_failed(uint32_t channel, char *line, size_t size)
{
    gFailedChannels.push_back(channel);
    snprintf(line, size, "%u", 160 + channel);
}

TEST_F(Host1xErrors, TheAttachUnmasksEveryChannelKeepsIntmaskBit4AndClearsWhatLatched)
{
    reg(kHintStatusExt) = 1u << 2;
    ASSERT_TRUE(start());
    EXPECT_TRUE(source()->enabled);
    EXPECT_EQ(reg(kIntMask), 0x11u) << "CPU0's master enable, bit 4 kept";
    EXPECT_EQ(reg(kHintMask), 1u << 31);
    EXPECT_EQ(reg(kHintMaskExt), 0x3fffu);
    EXPECT_EQ(reg(kHintStatusExt), 0u);
    EXPECT_TRUE(anyLine("latched before the general interrupt was on: channels 0x4")) << switch_test_iolog.size();
    EXPECT_TRUE(gFailedChannels.empty()) << "what latched before the channels were programmed fails nothing";
}

TEST_F(Host1xErrors, AnIntmaskThatDoesNotTakeLeavesItOffWithALine)
{
    gIntmaskStuck = true;
    EXPECT_FALSE(start());
    EXPECT_FALSE(source()->enabled);
    EXPECT_TRUE(anyLine("not enabled: INTMASK reads 0x10"));
}

TEST_F(Host1xErrors, NoInterruptNubLeavesItOffWithALine)
{
    gNoNub = true;
    EXPECT_FALSE(start());
    EXPECT_TRUE(anyLine("could not be attached"));
}

// The channel stays stopped, so its bit may latch again: taken once, masked until the channel's restart rearms it.
TEST_F(Host1xErrors, AStoppedChannelIsTakenOnceItsJobsFailAndItsRestartUnmasksIt)
{
    ASSERT_TRUE(start());
    reg(kCmdprocStat) = 1u << 1;
    ASSERT_TRUE(fire(1));
    EXPECT_EQ(gFailedChannels, std::vector<uint32_t>{1});
    EXPECT_EQ(reg(kHintMaskExt), 0x3fffu & ~(1u << 1));
    EXPECT_EQ(reg(kHintStatusExt), 0u);
    EXPECT_TRUE(anyLine("channel 1 stopped on an illegal opcode (CMDPROC_STAT 0x2): ch1 DMAGET 0x40; its jobs fail now "
                        "(syncpoints 161)"));
    EXPECT_FALSE(fire(1)) << "masked while stopped";
    EXPECT_EQ(gFailedChannels.size(), 1u);
    switch_host1x_errors_rearm(1);
    EXPECT_EQ(reg(kHintMaskExt), 0x3fffu);
    EXPECT_EQ(reg(kHintStatusExt), 0u) << "what latched meanwhile is cleared";
    EXPECT_TRUE(fire(1)) << "a later stop is taken again";
}

TEST_F(Host1xErrors, ARestartOfAChannelThatDidNotStopChangesNothing)
{
    ASSERT_TRUE(start());
    reg(kHintStatusExt) = 0;
    switch_host1x_errors_rearm(3);
    EXPECT_EQ(reg(kHintMaskExt), 0x3fffu);
}

TEST_F(Host1xErrors, SleepTurnsItOffAndTheResumeConfiguresItAgain)
{
    ASSERT_TRUE(start());
    switch_host1x_errors_stop();
    EXPECT_FALSE(source()->enabled);
    gRegs = {{kIntMask, 0x10}};  // SC7 reset the sync unit
    switch_host1x_errors_resume();
    EXPECT_TRUE(source()->enabled);
    EXPECT_EQ(reg(kHintMaskExt), 0x3fffu);
    EXPECT_EQ(reg(kIntMask), 0x11u);
}

// The illegal-opcode probe's error: taken by the filter, delivered to the probe, no job failed, no line.
TEST_F(Host1xErrors, AnExpectedErrorWakesTheProbeAndFailsNothing)
{
    ASSERT_TRUE(start());
    switch_host1x_errors_expect(13);
    switch_test_sleep = [](uint64_t) {
        EXPECT_TRUE(fire(13));
        return THREAD_AWAKENED;
    };
    EXPECT_EQ(switch_host1x_errors_expect_end(100), 1u);
    EXPECT_TRUE(gFailedChannels.empty());
    EXPECT_FALSE(anyLine("stopped on an illegal opcode"));
    switch_test_sleep = nullptr;
    reg(kCmdprocStat) = 0;
    switch_host1x_errors_rearm(13);
    EXPECT_TRUE(fire(13));
    EXPECT_EQ(gFailedChannels, std::vector<uint32_t>{13}) << "the expectation ended with the probe";
}

TEST_F(Host1xErrors, AnErrorThatLatchedAndWasNeverDeliveredPanicsAsLost)
{
    ASSERT_TRUE(start());
    switch_host1x_errors_expect(13);
    reg(kCmdprocStat) = 1u << 13;
    switch_test_sleep = [](uint64_t) {
        return THREAD_TIMED_OUT;
    };
    try {
        switch_host1x_errors_expect_end(100);
        ADD_FAILURE() << "no panic";
    } catch (const std::runtime_error &e) {
        EXPECT_EQ(std::string(e.what()), "TegraHost1x: general interrupt (INTID 99) lost: channel 13's illegal opcode "
                                         "latched (HINTSTATUS_EXT 0 CMDPROC_STAT 0x2000) and not delivered in 100 ms");
    }
    gExpectLock->held = false;
}

TEST_F(Host1xErrors, AnOpcodeThatIsNotIllegalLatchesNothingAndIsNoPanic)
{
    ASSERT_TRUE(start());
    switch_host1x_errors_expect(13);
    switch_test_sleep = [](uint64_t) {
        return THREAD_TIMED_OUT;
    };
    EXPECT_EQ(switch_host1x_errors_expect_end(100), 0u);
}
