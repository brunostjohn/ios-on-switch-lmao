// SPDX-License-Identifier: MIT
// Quiet job ends (SwitchGpuQuiet.h) need every kernel sleep on a user channel's fence to ask for its interrupt first,
// and every lost-wake panic to be keyed to the interrupt that wakes it, not to the fence passing the seqno.
// switch_gpu_quiet_test runs the protocol; these guard that each waiter takes part in it.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Body;
using switch_ios::test::Count;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

namespace
{
std::string unit(const char *name)
{
	return SourceText(std::string("../") + name);
}

void expectBefore(const std::string &text, const std::string &first, const std::string &second)
{
	const size_t a = text.find(first), b = text.find(second);
	ASSERT_NE(a, std::string::npos) << first;
	ASSERT_NE(b, std::string::npos) << second;
	EXPECT_LT(a, b) << first << " before " << second;
}
}  // namespace

TEST(GM20BQuietSource, TheFenceInterruptsOnlyWhenItAnnounces)
{
	const std::string fence = Body(unit("gpu_exec.c"), "static u32 chan_fence_job(");
	expectBefore(fence, "switch_gpu_quiet_announces(&chan->quiet, quiet)", "if (announce) {");
	EXPECT_EQ(Count(fence, "NON_STALL_INTERRUPT"), 1u);
	expectBefore(fence, "if (announce) {", "NON_STALL_INTERRUPT");
	expectBefore(fence, "switch_gpu_quiet_announce(&chan->quiet, next);", "PUSH_KICK(push);");
	const std::string exec =
		Body(unit("gpu_exec.c"),
		     "static inline __attribute__((always_inline)) int chan_exec_body(");
	EXPECT_THAT(exec, HasSubstr("(visibility & SWITCH_GPU_SUBMIT_QUIET) &&"));
	EXPECT_THAT(exec, HasSubstr("__atomic_load_n(&chan->wake, __ATOMIC_ACQUIRE)"));
	const std::string job = unit("TegraGM20BUserClientJobSubmit.cpp");
	EXPECT_THAT(job, HasSubstr("(head.flags & TegraGM20BJobQuiet) && !(head.flags & "
				   "TegraGM20BJobHasEvents)"));
}

TEST(GM20BQuietSource, EverySleepOnAUserChannelAsksFirst)
{
	const std::string chan = unit("gpu_chan.c"), sync = unit("gpu_sync.c"),
			  power = unit("gpu_power.c");
	expectBefore(Body(chan, "void tgm_chan_want("), "switch_gpu_waiter_want(",
		     "tgm_chan_need(chan, seqno);");
	const std::string del = Body(chan, "void tgm_chan_del(");
	expectBefore(del, "tgm_chan_quiet_wake(chan);", "nvxnu_idle(seq, end)");
	expectBefore(Body(sync, "static int sync_wait("), "tgm_chan_need_id(id, seqno);",
		     "nvxnu_idle(seq, end)");
	expectBefore(Body(sync, "static void writer_wait("), "tgm_chan_need_uid(uid, seqno);",
		     "nvxnu_idle(seq, end)");
	expectBefore(Body(power, "static void sleep_fences("), "tgm_chan_wake_push(chan);",
		     "nvxnu_idle(seq, end)");
	const std::string room = Body(unit("gpu_exec.c"), "int tgm_chan_wait_ready(");
	expectBefore(room, "switch_gpu_quiet_need(&chan->quiet, chan->submitted)",
		     "tgm_chan_wake_push(chan);");
	expectBefore(room, "tgm_chan_wake_push(chan);", "nvxnu_idle(seq, end)");
	EXPECT_THAT(
		Body(unit("SwitchGpuClientRetirement.h"), "void AGXDeviceUserClient::retire("),
		HasSubstr("tgm_chan_need(chans_[slot], uint32_t(record->snapshots[slot].value));"));
	expectBefore(unit("TegraGM20BUserClient.cpp"), "tgm_chan_want(chan, in->seqno);",
		     "tgm_waiter_idle(");
}

// A token is asked only of the GM20B channel it names, by uid and reset generation: a host1x engine's token holds a
// syncpoint in its queue. A bare uid is asked only by writer_wait, whose uids are GM20B channels'.
TEST(GM20BQuietSource, ATokenIsAskedOnlyOfItsChannel)
{
	expectBefore(Body(unit("gpu_quiet.c"), "void tgm_chan_need_tokens("),
		     "switch_gpu_quiet_token_on(&tokens[i], chan->id)",
		     "tgm_chan_need(chan, (u32)tokens[i].value);");
	size_t uids = 0;
	for (const char *name : {"gpu_bringup.c", "gpu_chan.c", "gpu_sync.c", "gpu_power.c", "gpu_exec.c",
				 "gpu_present.c", "gpu_scrub.c", "gpu_hang.c"})
		uids += Count(unit(name), "tgm_chan_need_uid(");
	EXPECT_EQ(uids, 1u) << "a token's channel is asked through tgm_chan_need_tokens";
	EXPECT_THAT(Body(unit("gpu_sync.c"), "static void writer_wait("),
		    HasSubstr("tgm_chan_need_uid(uid, seqno);"));
}

// A user channel's lost-wake panic fires once the interrupt that wakes the wait was due; the two left on the fence alone
// wait on kernel channels, which never end quiet.
TEST(GM20BQuietSource, LostWakePanicsWaitForTheAnnouncingInterrupt)
{
	const std::string chan = unit("gpu_chan.c"), sync = unit("gpu_sync.c"),
			  power = unit("gpu_power.c");
	EXPECT_THAT(Body(chan, "void tgm_chan_del("),
		    HasSubstr("tgm_chan_announced(chan, chan->submitted))\n\t\t\tnvxnu_wake_due("));
	EXPECT_THAT(Body(sync, "static int sync_wait("),
		    HasSubstr("tgm_chan_announced_id(id, seqno))"));
	EXPECT_THAT(Body(sync, "static void writer_wait("),
		    HasSubstr("tgm_chan_announced_uid(uid, seqno))"));
	EXPECT_THAT(Body(power, "static void sleep_fences("),
		    HasSubstr("tgm_chan_announced_id(id, seqno))"));
	EXPECT_THAT(
		Body(unit("TegraGM20BUserClient.cpp"), "IOReturn AGXDeviceUserClient::waitFence("),
		HasSubstr("tgm_chan_announced(chan, in->seqno)"));
	size_t panics = 0;
	for (const char *name :
	     {"gpu_bringup.c", "gpu_chan.c", "gpu_sync.c", "gpu_power.c", "gpu_exec.c",
	      "gpu_present.c", "gpu_scrub.c", "gpu_hang.c", "gpu_quiet.c"})
		panics += Count(unit(name), "nvxnu_wake_due(");
	EXPECT_EQ(panics, 6u) << "a new fence wait: ask for its interrupt and key its panic to it";
	EXPECT_THAT(Body(power, "static void wake_fence("),
		    HasSubstr("tgm_scrub.chan ? tgm_scrub.chan : tgm_present.chan"));
}

// The wake entry is pushed in SubmitJob's order (the channel's submit lock, then the submit gate) and inside the GPU's
// gate, so it never interleaves with a submission and never reaches a GPU that is asleep.
TEST(GM20BQuietSource, WakeEntriesKeepTheSubmissionOrder)
{
	const std::string wake =
		Body(unit("TegraGM20BUserClientWake.cpp"), "void AGXDeviceUserClient::pushWakes()");
	expectBefore(wake, "IOLockLock(submitLocks_[slot]);", "submitGate_.enterFast()");
	expectBefore(wake, "submitGate_.enterFast()", "tgm_chan_quiet_wake(chans_[slot]);");
	const std::string quiet = unit("gpu_quiet.c");
	expectBefore(Body(quiet, "void tgm_chan_quiet_wake("), "tgm_gate_enter();",
		     "tgm_chan_wake_push(chan);");
	const std::string push = Body(quiet, "void tgm_chan_wake_push(");
	expectBefore(push, "NON_STALL_INTERRUPT", "PUSH_KICK(push);");
	expectBefore(push, "switch_gpu_irq_arm(", "PUSH_KICK(push);");
	EXPECT_THAT(push, HasSubstr("nvxnu_panic("));
	EXPECT_THAT(Body(unit("TegraGM20BUserClientChannel.cpp"),
			 "IOReturn AGXDeviceUserClient::channelCreate("),
		    HasSubstr("tgm_chan_on_wake(chans_[slot], wakeAsked, this, slot);"));
	expectBefore(Body(unit("SwitchGpuClientRetirement.h"),
			  "void AGXDeviceUserClient::drainResources()"),
		     "tgm_chan_del(&chan);", "thread_call_cancel_wait(wakeCall_);");
}

// The idle clock counts a quiet job's seqnos once something reads its fence: its tick settles them when nothing was
// kicked for its drop time.
TEST(GM20BQuietSource, TheIdleClockSettlesQuietEnds)
{
	const std::string tick =
		Body(unit("SwitchGpuIdleDeadline.h"), "void tegra_gm20b_idle_tick(void)");
	expectBefore(tick, "if (idle_in_flight()) {", "nvxnu_work_schedule(tgm_idle.settle_w);");
	const std::string settle = Body(unit("gpu_quiet.c"), "void tgm_idle_settle(void *arg)");
	expectBefore(settle, "idle_retire(chan, tgm_chan_completed(chan));",
		     "tgm_chan_need(chan, READ_ONCE(chan->submitted));");
	EXPECT_THAT(settle, Not(HasSubstr("nvxnu_idle(")));
}
