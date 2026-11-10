// SPDX-License-Identifier: MIT
// SwitchGpuTimeline.cpp: a killed channel publishes its failure from gpu.c chan_killed, in atomic context, and every
// later status read sees it. Retiring what the failure left behind is the ownership layer's, in thread context.
#include <gtest/gtest.h>
#include "switch_gpu_timeline_stubs.h"
#include "drivers/TegraGM20B/SwitchGpuTimeline.h"
#include "drivers/TegraGM20B/TegraGM20BGpu.h"

namespace {
switch_gpu_backing_engine operations;
int fenceRefs;
} // namespace

extern "C" int switch_gpu_backing_register_engine(uint32_t engine, const switch_gpu_backing_engine *ops)
{
	if (engine != SWITCH_GPU_ENGINE_GM20B)
		return -22;
	operations = *ops;
	return 0;
}

extern "C" void switch_gpu_fence_hold(const volatile uint32_t *)
{
	++fenceRefs;
}

extern "C" void switch_gpu_fence_put(const volatile uint32_t *)
{
	--fenceRefs;
}

extern "C" void tgm_writer_wait(uint32_t, uint32_t)
{
	switchTimelineMayBlock();
}

TEST(Timeline, KillPublishesFailureWithoutSleepingOrAllocating)
{
	ASSERT_EQ(switch_gpu_timeline_init(), 0);
	volatile uint32_t fence = 0;
	auto *timeline = switch_gpu_timeline_create(7, &fence);
	ASSERT_NE(timeline, nullptr);
	switch_gpu_backing_token done = {}, lost = {};
	switch_gpu_timeline_token(timeline, 1, &done);
	switch_gpu_timeline_token(timeline, 2, &lost);
	ASSERT_EQ(operations.retain_token(&lost), 0);
	fence = 1;
	EXPECT_EQ(operations.status(&lost), 0);
	switchTimelineAtomicContext = true;
	switch_gpu_timeline_failed(timeline, fence);
	fence = 2;
	switchTimelineAtomicContext = false;
	EXPECT_EQ(operations.status(&done), 1);
	EXPECT_EQ(operations.status(&lost), -5);
	EXPECT_EQ(operations.wait(&lost), -5);
	switch_gpu_timeline_close(timeline);
	EXPECT_EQ(operations.status(&lost), -5);
	operations.release_token(&lost);
	EXPECT_EQ(fenceRefs, 0);
}
