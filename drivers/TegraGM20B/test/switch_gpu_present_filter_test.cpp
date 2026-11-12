// SPDX-License-Identifier: MIT
// SwitchGpuPresentFilter.cpp over the real timeline: the nonstall filter runs the display's callout once the watched
// producer is complete and visible, never takes a blocking lock, never after the claim's detach, and leaves a token
// whose status needs the timeline's lock (a semantic wait, a killed channel's abandoned fence) to the work loop.
#include <gtest/gtest.h>
#include <string>
#include <vector>
#include "bazel/testing/source_text.h"
#include "switch_gpu_timeline_stubs.h"
#include "drivers/TegraDisplayFlip/TegraScanoutCopy.h"
#include "drivers/TegraGM20B/SwitchGpuTimeline.h"
#include "drivers/TegraGM20B/TegraGM20BGpu.h"

struct switch_gpu_event_waits {
	unsigned refs = 1;
	int result = 0;
};

namespace
{
switch_gpu_backing_engine operations{};
uint64_t nextId = 0x5000;
std::vector<int> callouts;

void callout(void *ctx, int status)
{
	EXPECT_EQ(ctx, &callouts);
	EXPECT_TRUE(switchTimelineAtomicContext) << "outside the filter's simple lock";
	callouts.push_back(status);
}
}  // namespace

extern "C" int switch_gpu_backing_register_engine(uint32_t, const switch_gpu_backing_engine *ops)
{
	operations = *ops;
	return 0;
}

extern "C" int switch_gpu_backing_register_visibility(uint32_t,
						      const switch_gpu_backing_visibility *)
{
	return 0;
}

extern "C" int switch_gpu_backing_register_semantics(uint32_t, const switch_gpu_backing_semantics *)
{
	return 0;
}

extern "C" void switch_gpu_event_waits_retain(switch_gpu_event_waits *batch)
{
	++batch->refs;
}

extern "C" void switch_gpu_event_waits_release(switch_gpu_event_waits **batch)
{
	if (*batch) {
		--(*batch)->refs;
		*batch = nullptr;
	}
}

extern "C" int switch_gpu_event_waits_cached_status(const switch_gpu_event_waits *batch)
{
	return batch->result;
}

extern "C" int switch_gpu_event_waits_wait(switch_gpu_event_waits *batch, uint64_t)
{
	return batch->result;
}

extern "C" int switch_gpu_bridge_wait_external(const switch_gpu_backing_token *token)
{
	return switch_gpu_timeline_promote(token);
}

extern "C" void switch_gpu_fence_hold(const volatile uint32_t *)
{
}

extern "C" void switch_gpu_fence_put(const volatile uint32_t *)
{
}

extern "C" void switch_gpu_backing_completed()
{
}

extern "C" void tgm_writer_wait(uint32_t, uint32_t)
{
}

namespace
{
class PresentFilter : public testing::Test
{
protected:
	volatile uint32_t fence = 4, visible = 4;
	switch_gpu_timeline *timeline = nullptr;
	switch_gpu_backing_token watched{};

	void SetUp() override
	{
		ASSERT_EQ(switch_gpu_timeline_init(), 0);
		callouts.clear();
		tgm_present_filter_callout(callout, &callouts);
		timeline = switch_gpu_timeline_create(++nextId, &fence);
		ASSERT_NE(timeline, nullptr);
		switch_gpu_timeline_visibility(timeline, &visible);
		switch_gpu_timeline_token(timeline, 5, &watched);
		ASSERT_EQ(operations.retain_token(&watched), 0);
	}

	void TearDown() override
	{
		tgm_present_filter_detach();
		operations.release_token(&watched);
		switch_gpu_timeline_close(timeline);
	}

	static void filter()
	{
		switchTimelineAtomicContext = true;
		tgm_present_filter();
		switchTimelineAtomicContext = false;
	}
};

TEST_F(PresentFilter, TheCalloutRunsOnceThePassedFenceIsVisibleAndOnlyOnce)
{
	tgm_present_filter_attach(&watched);
	filter();
	__atomic_store_n(&fence, 5u, __ATOMIC_RELEASE);
	filter();
	EXPECT_TRUE(callouts.empty()) << "complete, its writes still in the L2";
	__atomic_store_n(&visible, 5u, __ATOMIC_RELEASE);
	filter();
	filter();
	EXPECT_EQ(callouts, std::vector<int>{0});
}

TEST_F(PresentFilter, NothingRunsAfterTheClaimDetaches)
{
	tgm_present_filter_attach(&watched);
	tgm_present_filter_detach();
	__atomic_store_n(&fence, 5u, __ATOMIC_RELEASE);
	__atomic_store_n(&visible, 5u, __ATOMIC_RELEASE);
	filter();
	EXPECT_TRUE(callouts.empty());
}

TEST_F(PresentFilter, AProducerAlreadyVisibleAtTheAttachRunsAtTheNextFilter)
{
	__atomic_store_n(&fence, 5u, __ATOMIC_RELEASE);
	__atomic_store_n(&visible, 5u, __ATOMIC_RELEASE);
	tgm_present_filter_attach(&watched);
	filter();
	EXPECT_EQ(callouts, std::vector<int>{0});
}

TEST_F(PresentFilter, AKilledChannelsAdvancedFenceIsNeverVisible)
{
	tgm_present_filter_attach(&watched);
	switch_gpu_timeline_failed(timeline, 4);
	__atomic_store_n(&fence, 100u, __ATOMIC_RELEASE);
	__atomic_store_n(&visible, 100u, __ATOMIC_RELEASE);
	filter();
	EXPECT_TRUE(callouts.empty());
	EXPECT_EQ(operations.status(&watched), -5);
}

TEST_F(PresentFilter, ASemanticWaitAtOrBelowTheTokenIsLeftToTheWorkLoop)
{
	switch_gpu_event_waits authority;
	switch_gpu_timeline_semantic *node = nullptr;
	ASSERT_EQ(switch_gpu_timeline_semantic_reserve(&watched, &authority, &node), 0);
	tgm_present_filter_attach(&watched);
	__atomic_store_n(&fence, 5u, __ATOMIC_RELEASE);
	__atomic_store_n(&visible, 5u, __ATOMIC_RELEASE);
	filter();
	EXPECT_TRUE(callouts.empty());
	switch_gpu_timeline_semantic_finish(&node, 0);
}

TEST_F(PresentFilter, AClosedChannelsTokenReadsWhatItCompletedBeforeTheClose)
{
	switch_gpu_backing_token earlier{};
	switch_gpu_timeline_token(timeline, 4, &earlier);
	tgm_present_filter_attach(&earlier);
	auto *closing = timeline;
	timeline = switch_gpu_timeline_create(++nextId, &fence);
	ASSERT_EQ(operations.retain_token(&earlier), 0);
	switch_gpu_timeline_close(closing);
	filter();
	EXPECT_EQ(callouts, std::vector<int>{0});
	tgm_present_filter_detach();
	operations.release_token(&earlier);
}

// The order SwitchGpuPresentFilter.cpp relies on: the view goes before the watch is armed and is dropped right after
// each claim, before the tokens drop and done runs; the nonstall line alone is filtered, masked until its pass.
TEST(PresentFilterSource, TheWatchAttachesBeforeItsArmAndEachClaimDetachesFirst)
{
	using switch_ios::test::Between;
	using switch_ios::test::SourceText;
	const std::string present = SourceText("../gpu_present.c");
	const std::string watch = Between(present, "int tgm_present_watch_dependencies(", "\n}\n");
	EXPECT_LT(watch.find("tgm_present_filter_attach(&tgm_present.tokens.tokens[0]);"),
		  watch.find("switch_gpu_present_watch_arm("));
	const std::string pass = Between(present, "void tgm_present_pass(void)\n{", "\n}\n");
	EXPECT_LT(pass.find("switch_gpu_present_watch_take_visible("),
		  pass.find("tgm_present_filter_detach();"));
	EXPECT_LT(pass.find("tgm_present_filter_detach();"), pass.find("present_watch_complete("));
	const std::string work =
		Between(present, "static void present_watch_work(void *unused)", "\n}\n");
	EXPECT_LT(work.find("switch_gpu_present_watch_claim("),
		  work.find("tgm_present_filter_detach();"));
	EXPECT_LT(work.find("tgm_present_filter_detach();"), work.find("present_watch_flush();"));
	const std::string glue = SourceText("../shim/xnu_glue.cpp");
	const std::string filterBody = Between(glue, "bool gpuNonstallFilter(", "\n}\n");
	EXPECT_NE(filterBody.find("tgm_present_filter();"), std::string::npos);
	EXPECT_NE(filterBody.find("return true;"), std::string::npos);
	EXPECT_NE(glue.find("gIrqLoop, gpuInterrupt, gpuNonstallFilter, nub, 0)"),
		  std::string::npos);
}
}  // namespace
