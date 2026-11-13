// SPDX-License-Identifier: MIT
// The kernel's CPU work per job, kick and interrupt pass, where no host model runs the unit: a job's kick publishes its
// GPFIFO entries with one barrier, and an interrupt pass wakes only the threads whose condition it met.
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Block;
using switch_ios::test::CodeText;
using switch_ios::test::SourceText;

namespace
{
std::string unit(const char *name)
{
	return SourceText(std::string("../") + name);
}
}  // namespace

// The kick's own store barrier (chan506f.c, switch_gpu_gpfifo_kick_test) is the job's only one.
TEST(GM20BFrameCpuSource, AJobsFenceAddsNoBarrierOfItsOwn)
{
	const CodeText fence = Block(unit("gpu_exec.c"), "static u32 chan_fence_job(");
	EXPECT_FALSE(fence.Has("dsb")) << "a barrier ahead of the kick's own";
	EXPECT_FALSE(fence.Has("dmb"));
}

// A pass wakes a thread on the global word only while one sleeps there, and moves a sequence it alone writes.
TEST(GM20BFrameCpuSource, ThePassWakesTheGlobalWordOnlyForASleeper)
{
	const std::string glue = unit("shim/xnu_glue.cpp");
	EXPECT_TRUE(Block(glue, "void wakeAfterPass()")
			    .Has("if (switch_gpu_wake_pass(&gWake)) wakeGlobal();"));
	EXPECT_TRUE(Block(glue, "extern \"C\" void nvxnu_wake(void)")
			    .Has("if (switch_gpu_wake_other(&gWake)) wakeGlobal();"));
	EXPECT_TRUE(Block(glue, "extern \"C\" int nvxnu_idle(nvxnu_u32 seq, nvxnu_u64 deadline_ns)")
			    .Before("switch_gpu_wake_sleep(&gWake, 1);", "idleOn("));
	EXPECT_TRUE(
		Block(glue, "void gpuInterrupt(OSObject *, IOInterruptEventSource *source, int)")
			.Has("wakeAfterPass();"));
}

// The kernel's waits on one channel's fence sleep on that channel's word, woken by the pass that moves its fence. The
// room wait stays on the global word: its GET report lands after the fence's release, so a pass between the two finds
// no room and the job's own pass sees no progress to wake it for.
TEST(GM20BFrameCpuSource, ChannelWaitsSleepOnTheirChannel)
{
	const CodeText room =
		Block(unit("gpu_exec.c"),
		      "int tgm_chan_wait_ready(struct tgm_chan *chan, u32 gpfifo_nr, u32 push_nr)");
	EXPECT_TRUE(room.Has("nvxnu_idle(seq, end)"));
	EXPECT_FALSE(room.Has("tgm_waiter_idle("));
	for (const char *wait : {"static int sync_wait(u64 id, u32 seqno, u64 end)",
				 "static void writer_wait(u32 uid, u32 seqno,"}) {
		const CodeText body = Block(unit("gpu_sync.c"), wait);
		EXPECT_FALSE(body.Has("nvxnu_idle(")) << wait;
		EXPECT_TRUE(body.Has("tgm_waiter_idle(")) << wait;
	}
	const CodeText del =
		Block(unit("gpu_chan.c"), "void tgm_chan_del(struct tgm_chan **pchan)");
	EXPECT_FALSE(del.Has("nvxnu_idle(")) << "the teardown's drain sleeps on the global word";
	EXPECT_TRUE(del.Has("tgm_waiter_idle("));
}

// A slow app's kick into a frame whose window was kept takes its holds after the kick's barrier and before its doorbell
// (gm20b_deadline_window_test runs the holds); only a user channel's kick looks.
TEST(GM20BFrameCpuSource, AnAppsFirstKickIsHeldBeforeItsDoorbell)
{
	const CodeText fence = Block(unit("gpu_exec.c"), "static u32 chan_fence_job(");
	EXPECT_TRUE(fence.Before("WRITE_ONCE(chan->submitted, next);",
				 "switch_gpu_busy_kick(&tgm_busy, chan->chid, next);"));
	EXPECT_TRUE(fence.Before("switch_gpu_busy_kick(&tgm_busy, chan->chid, next);",
				 "chan_doorbell(chan);"));
	const CodeText doorbell =
		Block(unit("gpu_exec.c"), "static void chan_doorbell(struct tgm_chan *chan)");
	EXPECT_TRUE(
		doorbell.Before("wmb();", "chan->app && !chan->high && tgm_deadline_app_kicked()"));
	EXPECT_TRUE(doorbell.Before("tgm_deadline_app_kicked()",
				    "writel_relaxed(chan->chan.gpfifo.cur,"));
	EXPECT_EQ(doorbell.Count("wmb();"), 2u)
		<< "the kick's one, and one after a hold's register writes";
}
