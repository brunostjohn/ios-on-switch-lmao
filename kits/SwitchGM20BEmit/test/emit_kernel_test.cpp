// SPDX-License-Identifier: MIT
// The kernel's hand-encoded host and copy pushes (TegraGM20BSyncpt.h, TegraGM20BRelayout.h), read back as methods and
// checked against the class headers' names and fields: what the kit's names will replace, method for method.
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "drivers/TegraGM20B/TegraGM20BRelayout.h"
#include "drivers/TegraGM20B/TegraGM20BSyncpt.h"
#include "kits/SwitchGM20B/SwitchGM20B.h"
#include "push_words.h"

namespace {

using switch_gm20b_test::Method;
using switch_gm20b_test::Methods;

constexpr uint32_t kHost = SUBC_NVB06F, kCopy = SUBC_NVB0B5;

std::vector<uint32_t> words(uint32_t n, const uint32_t *w)
{
    return std::vector<uint32_t>(w, w + n);
}

TEST(EmitKernel, SyncpointWaitIsTheTegraHostMethods)
{
    uint32_t w[TGM_SYNCPT_WAIT_DWORDS];
    const uint32_t n = tgm_syncpt_wait(w, 17, 1234);
    const uint32_t wait =
        uint32_t(NVVAL(SWITCH_NVB06F, SYNCPOINTB, SYNCPT_INDEX, 17) |
                 NVDEF(SWITCH_NVB06F, SYNCPOINTB, WAIT_SWITCH, EN) | NVDEF(SWITCH_NVB06F, SYNCPOINTB, OPERATION, WAIT));
    EXPECT_EQ(Methods(words(n, w)),
              (std::vector<Method>{{kHost, SWITCH_NVB06F_SYNCPOINTA, 1234}, {kHost, SWITCH_NVB06F_SYNCPOINTB, wait}}));
}

TEST(EmitKernel, SyncpointIncrementFlushesThenIncrements)
{
    uint32_t w[TGM_SYNCPT_INCR_DWORDS];
    const uint32_t n = tgm_syncpt_incr(w, 17);
    const uint32_t incr = uint32_t(NVVAL(SWITCH_NVB06F, SYNCPOINTB, SYNCPT_INDEX, 17) |
                                   NVDEF(SWITCH_NVB06F, SYNCPOINTB, OPERATION, INCR));
    std::vector<Method> expected = {
        {kHost, NVB06F_WFI, 0},
        {kHost, NVB06F_FB_FLUSH, 0},
        {kHost, NVB06F_MEM_OP_C, 0},
        {kHost, NVB06F_MEM_OP_D, uint32_t(NVDEF(NVB06F, MEM_OP_D, OPERATION, L2_FLUSH_DIRTY))},
        {kHost, NVB06F_FB_FLUSH, 0},
        {kHost, SWITCH_NVB06F_SYNCPOINTA, 0},
    };
    for (uint32_t k = 0; k < TGM_SYNCPT_INCRS; k++)
        expected.push_back({kHost, SWITCH_NVB06F_SYNCPOINTB, incr});
    EXPECT_EQ(Methods(words(n, w)), expected);
}

TEST(EmitKernel, RelayoutAcquireIsAnAcquireGeqThatSwitches)
{
    const tgm_relayout_wait wait = {0x12345678c0, 77, 3};
    uint32_t w[5];
    const uint32_t n = tgm_relayout_acquire(w, &wait);
    const uint32_t d =
        uint32_t(NVDEF(NVB06F, SEMAPHORED, OPERATION, ACQ_GEQ) | NVDEF(NVB06F, SEMAPHORED, ACQUIRE_SWITCH, ENABLED));
    EXPECT_EQ(Methods(words(n, w)), (std::vector<Method>{{kHost, NVB06F_SEMAPHOREA, 0x12},
                                                         {kHost, NVB06F_SEMAPHOREB, 0x345678c0},
                                                         {kHost, NVB06F_SEMAPHOREC, 77},
                                                         {kHost, NVB06F_SEMAPHORED, d}}));
}

TEST(EmitKernel, RelayoutClearIsAConstantRemapFill)
{
    uint32_t w[TGM_RELAYOUT_DWORDS];
    const uint32_t n = tgm_relayout_clear(w, 0x4400001000, 0x30000);
    const uint32_t remap = uint32_t(NVDEF(NVB0B5, SET_REMAP_COMPONENTS, DST_X, CONST_A) |
                                    NVDEF(NVB0B5, SET_REMAP_COMPONENTS, COMPONENT_SIZE, FOUR) |
                                    NVDEF(NVB0B5, SET_REMAP_COMPONENTS, NUM_SRC_COMPONENTS, ONE) |
                                    NVDEF(NVB0B5, SET_REMAP_COMPONENTS, NUM_DST_COMPONENTS, ONE));
    const uint32_t launch = uint32_t(
        NVDEF(NVB0B5, LAUNCH_DMA, DATA_TRANSFER_TYPE, NON_PIPELINED) | NVDEF(NVB0B5, LAUNCH_DMA, FLUSH_ENABLE, TRUE) |
        NVDEF(NVB0B5, LAUNCH_DMA, SRC_MEMORY_LAYOUT, PITCH) | NVDEF(NVB0B5, LAUNCH_DMA, DST_MEMORY_LAYOUT, PITCH) |
        NVDEF(NVB0B5, LAUNCH_DMA, MULTI_LINE_ENABLE, TRUE) | NVDEF(NVB0B5, LAUNCH_DMA, REMAP_ENABLE, TRUE));
    EXPECT_EQ(Methods(words(n, w)), (std::vector<Method>{{kCopy, NVB0B5_SET_REMAP_CONST_A, 0},
                                                         {kCopy, NVB0B5_SET_REMAP_CONST_B, 0},
                                                         {kCopy, NVB0B5_SET_REMAP_COMPONENTS, remap},
                                                         {kCopy, NVB0B5_OFFSET_OUT_UPPER, 0x44},
                                                         {kCopy, NVB0B5_OFFSET_OUT_LOWER, 0x1000},
                                                         {kCopy, NVB0B5_PITCH_IN, 4096},
                                                         {kCopy, NVB0B5_PITCH_OUT, 4096},
                                                         {kCopy, NVB0B5_LINE_LENGTH_IN, 1024},
                                                         {kCopy, NVB0B5_LINE_COUNT, 0x30},
                                                         {kCopy, NVB0B5_LAUNCH_DMA, launch}}));
}

}  // namespace
