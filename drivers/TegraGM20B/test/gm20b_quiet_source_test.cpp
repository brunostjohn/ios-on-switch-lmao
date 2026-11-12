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
	// a semantic fence bound to the job (its own, or one a dependency passed on) resolves at a published completion, and
	// a watched present's producer writes a backing the display scans in place
	expectBefore(job, "error = events->bind(&completion);", "switch_gpu_quiet_end(quiet, events->semanticFence(),");
	expectBefore(job, "scanoutWrites |= switch_gpu_backing_scanned(bo->backing) != 0;",
		     "switch_gpu_quiet_end(quiet, events->semanticFence(),");
	EXPECT_THAT(job, HasSubstr("scanoutWrites, closesGroup);"));
	EXPECT_THAT(job, HasSubstr("!(head.flags & TegraGM20BJobAdmission) || (head.flags & TegraGM20BJobAdmissionEnd);"));
	expectBefore(job, "const bool quietEnd", "(quietEnd ? uint32_t(SWITCH_GPU_SUBMIT_QUIET) : 0u)");
	EXPECT_THAT(job, HasSubstr("(jobVisibility & SWITCH_GPU_SUBMIT_QUIET) && count ? 1 : 0"));
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

// Writer promises (SwitchGpuWriterPromise.h): a present and another connection's job wait for them on the GPU; the job
// that keeps one releases it after its own fence; the CPU releases the rest; nothing waits for its own promise; no lease
// a present or a display slot took behind one holds the promised job's write off; a CPU reader waits for the promised
// job.
TEST(GM20BPromiseSource, PresentsAndJobsOrderBehindWriterPromises)
{
	expectBefore(Body(unit("SwitchGpuScanoutSnapshot.h"), "int add(switch_gpu_backing *backing)"),
		     "switch_gpu_backing_snapshot_read_promises(", "tokens + count,");
	const std::string job = unit("TegraGM20BUserClientJobSubmit.cpp");
	expectBefore(job, "keepPromises(record.promises, releases)",
		     "switch_gpu_chan_submit_enter(chan);");
	expectBefore(job, "events->captureDependencies(job.dependencies, dependencies);",
		     "switch_gpu_backing_promise_dependencies(&transaction, job.dependencies,");
	expectBefore(job, "switch_gpu_backing_promise_dependencies(&transaction, job.dependencies,",
		     "switch_gpu_chan_exec_job(");
	// between begin and commit the transaction holds every use's backing lock: a promise read that takes one again
	// (switch_gpu_backing_promise_tokens) never returns on XNU
	EXPECT_THAT(job, Not(HasSubstr("switch_gpu_backing_promise_tokens(")));
	expectBefore(job, "switch_gpu_backing_commit(&transaction);",
		     "keptPromises(record.promises, slot, completion);");
	const std::string fence = Body(unit("gpu_exec.c"), "static u32 chan_fence_job(");
	expectBefore(fence, "lower_32_bits(chan->fence_gpu)", "releases[s].value");
	const std::string acquire = Body(unit("gpu_exec.c"), "int tgm_backing_dependencies(");
	expectBefore(acquire, "token->engine == SWITCH_GPU_ENGINE_PROMISE",
		     "tgm_chan_note_promise(chan, token->queue,");
	expectBefore(Body(unit("gpu_hang.c"), "enum tgm_hang tgm_hang_recover("),
		     "tgm_chan_release_promises(chans[i], who, waited_ns);",
		     "verdict = tgm_hang_blame(");
	// on a confirmed strike only: the reader's wait on a word no live channel owns is a victim's, which the blame rule
	// spares until then, so no first look gives the promise up
	expectBefore(Body(unit("gpu_hang.c"), "enum tgm_hang tgm_hang_recover("),
		     "if (chans[i] && c[i].stuck && !c[i].moving)",
		     "tgm_chan_release_promises(chans[i], who, waited_ns);");
	// The hang rule's compare-and-swap is a store-release to the write-combined fence page, which the parked PBDMA
	// reads from DRAM (volatile, no L2): a DSB completes it, as it does after the CPU's own release (releasePromise).
	expectBefore(Body(unit("gpu_promise.c"), "uint32_t tgm_chan_release_promises("),
		     "switch_gpu_promise_waits_release(&chan->promise_waits,", "__asm__ volatile(\"dsb sy\" ::: \"memory\");");
	EXPECT_THAT(Body(unit("SwitchGpuWriterPromise.h"), "static inline uint32_t switch_gpu_promise_waits_release("),
		    HasSubstr("__ATOMIC_RELEASE"));
	// switch_gpu_promise_strike_test models these steps: the mark first, taken back if the maker won, one line else
	const std::string released = Body(unit("gpu_promise.c"), "static void promise_released(");
	expectBefore(released, "step == SWITCH_GPU_PROMISE_RAISING", "tgm_promise_abandon(offset, value);");
	expectBefore(released, "step == SWITCH_GPU_PROMISE_KEPT_FIRST",
		     "switch_gpu_promise_abandoned_drop(&promise_abandoned, offset, value);");
	expectBefore(released, "switch_gpu_promise_abandoned_drop(", "tgm_promise_abandoned_report(");
	EXPECT_EQ(Count(released, "tgm_promise_abandoned_report("), 1u);
	// a present whose job parks on an unkept promise is first looked at once stuck for TGM_STUCK_NS and struck again
	// TGM_STUCK_NS later, a real hang at the deadline and a whole deadline later; the deadline's own wait is that
	// interval (switch_gpu_promise_strike_test models both)
	const std::string copy = Body(unit("gpu_present.c"), "static int present_copy(");
	expectBefore(copy, "switch_gpu_present_tokens_promised(&tgm_present.tokens) ? TGM_PROMISE_FIRST_LOOK_NS",
		     "nvxnu_work_schedule_after(tgm_present.deadline_work, tgm_present.recheck);");
	expectBefore(copy, "nvxnu_work_schedule_after(tgm_present.deadline_work, tgm_present.recheck);",
		     "chan_fence(chan);");
	const std::string deadline = Body(unit("gpu_present_deadline.c"), "void tgm_present_deadline(void *arg)");
	expectBefore(deadline, "if (age < tgm_present.recheck) {",
		     "tgm_hang_recover(late, \"present\"");
	expectBefore(deadline, "tgm_present.recheck = tgm_chan_waits_promise(late) ? TGM_STUCK_NS : TGM_PRESENT_NS;",
		     "nvxnu_work_schedule_after(tgm_present.deadline_work, tgm_present.recheck);");
	EXPECT_THAT(unit("SwitchGpuAdmission.cpp"), HasSubstr("switch_gpu_backing_exclusive_readers(backing)"));
	EXPECT_THAT(unit("SwitchGpuOwnership.cpp"), HasSubstr("switch_gpu_backing_exclusive_readers(backing)"));
	EXPECT_THAT(unit("SwitchGpuCpuAccess.cpp"), HasSubstr("--backing->promised_readers;"));
	EXPECT_THAT(Body(unit("SwitchGpuCpuAccess.cpp"), "bool cpu_conflicts("),
		    HasSubstr("switch_gpu_backing_promise_pending_locked(backing)"));
	expectBefore(Body(unit("SwitchGpuCpuLease.cpp"), "static int snapshot_read("), "while (!avoid &&",
		     "(cpu && switch_gpu_backing_promise_pending_locked(backing))))");
	// an engine reader of a promised surface (VIC, NVDEC) orders behind the promise through the bridge
	for (const char *reader : {"../TegraVIC/SwitchVICJobs.cpp", "../TegraNVDEC/SwitchNVDECJobs.cpp"})
		expectBefore(unit(reader), "switch_gpu_backing_promise_dependencies(&transaction,",
			     "dependency.engine == SWITCH_GPU_ENGINE_PROMISE");
	const std::string retirement = unit("SwitchGpuClientRetirement.h");
	expectBefore(Body(retirement, "void AGXDeviceUserClient::drainResources()"),
		     "releasePromises(true);", "tgm_chan_del(&chan);");
	EXPECT_THAT(Body(retirement, "void AGXDeviceUserClient::retireBatch()"),
		    HasSubstr("releasePromises(false);"));
	EXPECT_THAT(Body(unit("TegraGM20BUserClientChannel.cpp"),
			 "IOReturn AGXDeviceUserClient::channelDestroy("),
		    HasSubstr("releasePromises(false);"));
}
