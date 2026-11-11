// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "switch_host1x_submit_stubs.h"
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <array>
#include <vector>
#include "bazel/testing/source_text.h"
#define SWITCH_DCACHE_OP(op, address) ((void)(address))
#define SWITCH_DCACHE_BARRIER()       ((void)0)
#include "../TegraHost1x.cpp"

namespace {
alignas(16384) uint8_t gHostRegisters[kHost1xSize];
alignas(4096) uint8_t gMcRegisters[0x1000];
alignas(4096) uint8_t gPushBuffers[TEGRA_HOST1X_CHANNELS][kPushBytes + PAGE_SIZE];
uint32_t gPushAllocated;
std::vector<std::array<uint32_t, TEGRA_HOST1X_CHANNELS>> gDemands;  // every channel's DMAPUT at each demand

uint32_t doorbell(uint32_t channel)
{
    return switch_mmio_read32(gHostRegisters, channel * kChannelStride + CH_DMAPUT);
}
}  // namespace

extern "C" uintptr_t ml_io_map(uintptr_t pa, uintptr_t)
{
    return reinterpret_cast<uintptr_t>(pa == kHost1xPA ? gHostRegisters : gMcRegisters);
}

extern "C" bool switch_car_clock_enabled(unsigned)
{
    return true;
}

extern "C" bool switch_car_reset_asserted(unsigned)
{
    return false;
}

extern "C" uint32_t switch_car_read(uint32_t)
{
    return SOURCE_PLLP | HOST1X_DIV_2_5;
}

extern "C" void switch_car_write(uint32_t, uint32_t)
{}

extern "C" void switch_car_clock_enable(unsigned)
{}

extern "C" void switch_car_reset_deassert(unsigned)
{}

bool switch_dma_alloc(switch_dma_buffer *out, size_t size, uint64_t, IOOptionBits)
{
    if (gPushAllocated == TEGRA_HOST1X_CHANNELS || size > sizeof(gPushBuffers[0]))
        return false;
    out->va = gPushBuffers[gPushAllocated++];
    out->pa = 0x80000000u + gPushAllocated * 0x10000u;
    out->size = size;
    return true;
}

IOService *switch_interrupt_nub(uint32_t, bool)
{
    static IOService nub;
    return &nub;
}

void switch_interrupt_describe(uint32_t, char *buffer, size_t size)
{
    snprintf(buffer, size, "host test");
}

bool switch_host1x_intr_init(IOWorkLoop *, volatile uint8_t *, uint32_t)
{
    return true;
}

void switch_host1x_intr_start()
{}

void switch_host1x_intr_stop()
{}

void switch_host1x_intr_handle()
{}

extern "C" void switch_emc_gpu_submit(void)
{
    std::array<uint32_t, TEGRA_HOST1X_CHANNELS> puts{};
    for (uint32_t channel = 0; channel < TEGRA_HOST1X_CHANNELS; channel++)
        puts[channel] = doorbell(channel);
    gDemands.push_back(puts);
}

TEST(Host1xSubmit, EveryEngineRaisesTheMemoryClockBeforeItsDoorbell)
{
    ASSERT_EQ(tegra_host1x_start(), TEGRA_HOST1X_OK);
    const uint32_t words[] = {tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_IMMEDIATE, 170), kOpNop, kOpNop};

    const struct {
        const char *engine;
        uint32_t channel;
    } engines[] = {{"NVDEC", TEGRA_HOST1X_CHANNEL_DEFAULT},
                   {"VIC", TEGRA_HOST1X_CHANNEL_VIC},
                   {"NVENC", TEGRA_HOST1X_CHANNEL_NVENC},
                   {"NVJPG", TEGRA_HOST1X_CHANNEL_NVJPG}};

    for (const auto &e : engines) {
        gDemands.clear();
        const uint32_t before = doorbell(e.channel);
        const int submitted = e.channel == TEGRA_HOST1X_CHANNEL_DEFAULT ? tegra_host1x_submit(words, 3)
                                                                        : tegra_host1x_submit_on(e.channel, words, 3);
        ASSERT_EQ(submitted, TEGRA_HOST1X_OK) << e.engine;
        EXPECT_EQ(doorbell(e.channel), before + sizeof(words)) << e.engine;
        ASSERT_EQ(gDemands.size(), 1u) << e.engine << ": a submission registers EMC demand once";
        EXPECT_EQ(gDemands[0][e.channel], before) << e.engine << ": demand after the doorbell";
    }
    gDemands.clear();
    EXPECT_EQ(tegra_host1x_submit_on(TEGRA_HOST1X_CHANNELS, words, 3), TEGRA_HOST1X_EINVAL);
    EXPECT_TRUE(gDemands.empty());
}

// Each engine's jobs reach host1x through the submission above, so they register the same demand.
TEST(Host1xSubmit, EveryEngineSubmitsThroughHost1x)
{
    using switch_ios::test::SourceText;
    EXPECT_THAT(SourceText("../../TegraNVDEC/SwitchNVDECJobs.cpp"),
                testing::HasSubstr("tegra_host1x_submit(work.words"));
    EXPECT_THAT(SourceText("../../TegraVIC/SwitchVICJobs.cpp"),
                testing::HasSubstr("tegra_host1x_submit_on(TEGRA_HOST1X_CHANNEL_VIC, words, n)"));
    EXPECT_THAT(SourceText("../../TegraNVENC/TegraNVENC.cpp"),
                testing::HasSubstr("tegra_host1x_submit_on(TEGRA_HOST1X_CHANNEL_NVENC, words + first, n - first)"));
    EXPECT_THAT(SourceText("../../TegraNVJPG/TegraNVJPG.cpp"),
                testing::HasSubstr("tegra_host1x_submit_on(TEGRA_HOST1X_CHANNEL_NVJPG, words, n)"));
}
