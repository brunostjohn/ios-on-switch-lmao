// SPDX-License-Identifier: BSD-2-Clause-Patent
// TegraHost1x.cpp and SwitchHost1xFence.cpp on the host with a memory register file: where waits sleep and callbacks run.
// The memory does not clear status bits written one, so each step sets the registers the interrupt would see.
#include "switch_host1x_fence_stubs.h"
#include <gtest/gtest.h>
#include <string>
#include <vector>
#define SWITCH_DCACHE_OP(op, address) ((void)(address))
#define SWITCH_DCACHE_BARRIER()       ((void)0)
#include "../TegraHost1x.cpp"
#include "../SwitchHost1xFence.cpp"

namespace {
alignas(16384) uint8_t gHostRegisters[kHost1xSize];
alignas(4096) uint8_t gMcRegisters[0x1000];
alignas(4096) uint8_t gPushBuffers[TEGRA_HOST1X_CHANNELS][kPushBytes + PAGE_SIZE];
uint32_t gPushAllocated;

uint32_t &reg(uint32_t offset)
{
    return *reinterpret_cast<uint32_t *>(gHostRegisters + kSyncOffset + offset);
}

uint32_t &value(uint32_t id)
{
    return reg(0xf80 + 4 * id);
}

uint32_t &threshold(uint32_t id)
{
    return reg(0x1380 + 4 * id);
}

// What the threshold interrupt's line would carry for id: its status bit, and then the action on the work loop.
void interrupt(uint32_t id)
{
    reg(0xe80 + 4 * (id / 32)) = 1u << (id % 32);
    switch_host1x_intr_handle();
    reg(0xe80 + 4 * (id / 32)) = 0;
}

struct Signal {
    int calls = 0, status = 1;
    bool lockHeld = false;
};

std::vector<Signal> gSignals(8);

void signaled(switch_host1x_fence *fence, int status)
{
    auto &signal = gSignals[fence->threshold % gSignals.size()];
    ++signal.calls;
    signal.status = status;
    signal.lockHeld = gLock->held;
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

extern "C" void switch_emc_gpu_submit(void)
{}

namespace {
struct Host1xFence : testing::Test {
    void SetUp() override
    {
        ASSERT_EQ(tegra_host1x_start(), TEGRA_HOST1X_OK);
        switch_test_sleep = nullptr;
        switch_test_kicks = switch_test_wakeups = 0;
        switch_test_now = 1000;
        switch_test_bounds_at = 0;
        gBoundsAt = 0;
        gSignals.assign(8, {});
    }
};

TEST_F(Host1xFence, AWaitOnAReachedSyncpointDoesNotSleep)
{
    value(165) = 7;
    EXPECT_EQ(tegra_host1x_syncpt_wait(165, 7, 1000), TEGRA_HOST1X_OK);
}

// NVENC waits from two places on one syncpoint: a waiter's fence leaves the others armed.
TEST_F(Host1xFence, AWaiterEndsByTheInterruptAndLeavesTheOtherFencesOfItsSyncpointArmed)
{
    value(166) = 0;
    switch_host1x_fence later;
    switch_host1x_fence_init(&later, 166, 5, signaled);
    ASSERT_EQ(switch_host1x_fence_add(&later, 0), TEGRA_HOST1X_OK);
    int sleeps = 0;
    switch_test_sleep = [&](uint64_t) {
        ++sleeps;
        value(166) = 3;
        interrupt(166);
        return THREAD_AWAKENED;
    };
    EXPECT_EQ(tegra_host1x_syncpt_wait(166, 3, 1000), TEGRA_HOST1X_OK);
    EXPECT_EQ(sleeps, 1);
    EXPECT_EQ(threshold(166), 5u);
    EXPECT_EQ(later.state, SWITCH_HOST1X_FENCE_QUEUED);
    EXPECT_EQ(gSignals[5].calls, 0);
    value(166) = 5;
    interrupt(166);
    EXPECT_EQ(gSignals[5].calls, 1);
    EXPECT_EQ(gSignals[5].status, 0);
    EXPECT_FALSE(gSignals[5].lockHeld);
}

TEST_F(Host1xFence, AWaitTheEngineNeverFinishesTimesOutAndDisarms)
{
    value(167) = 0;
    switch_test_sleep = [](uint64_t) { return THREAD_TIMED_OUT; };
    EXPECT_EQ(tegra_host1x_syncpt_wait(167, 1, 1000), TEGRA_HOST1X_ETIMEDOUT);
    EXPECT_EQ(reg(0xf00 + 4 * (167 / 32)) & (1u << (167 % 32)), 1u << (167 % 32));
}

TEST_F(Host1xFence, AReachedWaitTheInterruptMissedIsLostAfterOneGrace)
{
    value(168) = 0;
    int sleeps = 0;
    switch_test_sleep = [&](uint64_t) {
        ++sleeps;
        value(168) = 1;
        return THREAD_TIMED_OUT;
    };
    EXPECT_EQ(tegra_host1x_syncpt_wait(168, 1, 1000), TEGRA_HOST1X_ELOST);
    EXPECT_EQ(sleeps, 2);
}

TEST_F(Host1xFence, OnceStrictALostInterruptPanicsWithTheSyncpointAndTheGic)
{
    tegra_host1x_set_strict();
    value(169) = 0;
    switch_test_sleep = [&](uint64_t) {
        value(169) = 1;
        return THREAD_TIMED_OUT;
    };
    try {
        tegra_host1x_syncpt_wait(169, 1, 1000);
        ADD_FAILURE() << "no panic";
    } catch (const std::runtime_error &e) {
        EXPECT_NE(std::string(e.what()).find("syncpoint 169 threshold interrupt lost (INTID 97)"), std::string::npos)
            << e.what();
        EXPECT_NE(std::string(e.what()).find("host test"), std::string::npos);
    }
    // The panic left the waiter's fence, on the gone stack, in the list.
    __atomic_store_n(&gStrict, false, __ATOMIC_RELEASE);
    gLock->held = false;
    gList = SwitchHost1xFenceList<Registers>{};
}

TEST_F(Host1xFence, AFenceAlreadyReachedIsHandedToTheWorkLoopNotCalledInline)
{
    value(170) = 9;
    switch_host1x_fence fence;
    switch_host1x_fence_init(&fence, 170, 2, signaled);
    ASSERT_EQ(switch_host1x_fence_add(&fence, 0), TEGRA_HOST1X_OK);
    EXPECT_EQ(gSignals[2].calls, 0);
    EXPECT_EQ(fence.state, SWITCH_HOST1X_FENCE_HANDED_OFF);
    EXPECT_EQ(switch_test_kicks, 1u);
    switch_host1x_intr_handle();
    EXPECT_EQ(gSignals[2].calls, 1);
    EXPECT_EQ(gSignals[2].status, 0);
    EXPECT_EQ(fence.state, SWITCH_HOST1X_FENCE_IDLE);
}

TEST_F(Host1xFence, AHandedOffFenceRemovedBeforeTheWorkLoopRunsIsNeverCalled)
{
    value(171) = 4;
    switch_host1x_fence fence;
    switch_host1x_fence_init(&fence, 171, 3, signaled);
    ASSERT_EQ(switch_host1x_fence_add(&fence, 0), TEGRA_HOST1X_OK);
    EXPECT_TRUE(switch_host1x_fence_remove(&fence));
    switch_host1x_intr_handle();
    EXPECT_EQ(gSignals[3].calls, 0);
    EXPECT_FALSE(switch_host1x_fence_remove(&fence));
}

TEST_F(Host1xFence, ABoundOutlivedFailsTheFenceOnTheWorkLoop)
{
    value(172) = 0;
    switch_host1x_fence fence;
    switch_host1x_fence_init(&fence, 172, 4, signaled);
    ASSERT_EQ(switch_host1x_fence_add(&fence, 2000), TEGRA_HOST1X_OK);
    EXPECT_EQ(switch_test_bounds_at, 1000u + 2000u * 1000u);
    switch_test_now = switch_test_bounds_at;
    switch_test_bounds(nullptr, nullptr);
    EXPECT_EQ(gSignals[4].calls, 0);
    switch_host1x_intr_handle();
    EXPECT_EQ(gSignals[4].calls, 1);
    EXPECT_EQ(gSignals[4].status, TEGRA_HOST1X_ETIMEDOUT);
}

// The bound of a job behind a GPU producer starts when the producer is done (D8): watch moves the deadline.
TEST_F(Host1xFence, WatchStartsTheBoundOfAnUnboundedFence)
{
    value(173) = 0;
    switch_host1x_fence fence;
    switch_host1x_fence_init(&fence, 173, 6, signaled);
    ASSERT_EQ(switch_host1x_fence_add(&fence, 0), TEGRA_HOST1X_OK);
    EXPECT_EQ(switch_test_bounds_at, 0u);
    switch_test_now = 5000;
    switch_host1x_fence_watch(&fence, 10);
    EXPECT_EQ(switch_test_bounds_at, 5000u + 10u * 1000u);
    EXPECT_TRUE(switch_host1x_fence_remove(&fence));
}

TEST_F(Host1xFence, AcrossSleepAddsFailAndPendingFencesAreArmedAgain)
{
    value(174) = 0;
    switch_host1x_fence fence, refused;
    switch_host1x_fence_init(&fence, 174, 1, signaled);
    switch_host1x_fence_init(&refused, 174, 7, signaled);
    ASSERT_EQ(switch_host1x_fence_add(&fence, 0), TEGRA_HOST1X_OK);
    tegra_host1x_suspend();
    EXPECT_EQ(switch_host1x_fence_add(&refused, 0), TEGRA_HOST1X_ENODEV);
    EXPECT_EQ(tegra_host1x_syncpt_wait(174, 1, 1000), TEGRA_HOST1X_ENODEV);
    threshold(174) = 0;
    tegra_host1x_resume();
    EXPECT_EQ(threshold(174), 1u);
    EXPECT_EQ(fence.state, SWITCH_HOST1X_FENCE_QUEUED);
    value(174) = 1;
    interrupt(174);
    EXPECT_EQ(gSignals[1].calls, 1);
}
}  // namespace
