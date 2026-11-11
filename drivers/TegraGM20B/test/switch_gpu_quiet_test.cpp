// SPDX-License-Identifier: MIT
// Quiet job ends on one channel as gpu_exec.c, gpu_quiet.c and the interrupt pass drive them: the GPU runs the channel's
// entries in order, a fence or wake entry that interrupts runs the pass, and a waiter publishes its seqno and asks
// before it tests the fence.
#include <gtest/gtest.h>
#include <deque>
#include "../SwitchGpuActiveIrq.h"
#include "../SwitchGpuJobFences.h"
#include "../SwitchGpuQuiet.h"

namespace
{
struct Channel {
	struct Entry {
		uint32_t seqno;  // the fence's release; 0 for a wake entry
		bool interrupts;
	};

	explicit Channel(uint32_t start) : submitted(start), fence(start), seen(start)
	{
		switch_gpu_quiet_init(&quiet, start);
		switch_gpu_jobs_init(&jobs, start);
	}

	// chan_fence_job's decision and its kick, for a job releasing `seqnos`
	uint32_t submit(uint32_t seqnos, bool quietJob)
	{
		const int announce = switch_gpu_quiet_announces(&quiet, quietJob);
		return kick(seqnos, announce);
	}

	uint32_t kick(uint32_t seqnos, int announce)
	{
		const uint32_t first = submitted + 1, last = submitted + seqnos;
		switch_gpu_jobs_kick(&jobs, first, last, 0, !announce);
		if (announce)
			switch_gpu_quiet_announce(&quiet, last);
		fifo.push_back({last, announce != 0});
		submitted = last;
		return last;
	}

	// The owner's thread call (tgm_chan_wake_push): an entry behind the newest job unless the pass saw it end
	void ownerWake()
	{
		asked = false;
		if (!switch_gpu_quiet_wake_due(&quiet, submitted))
			return;
		switch_gpu_quiet_announce(&quiet, submitted);
		if (int32_t(seen - submitted) >= 0)
			return;
		switch_gpu_jobs_announce_newest(&jobs);
		fifo.push_back({0, true});
		++wakeEntries;
	}

	// tgm_chan_want: the waiter's slot, then the ask; true if it sleeps
	bool wait(uint32_t seqno)
	{
		switch_gpu_waiter_want(&wanted, seqno);
		if (int32_t(fence - seqno) < 0 && switch_gpu_quiet_need(&quiet, seqno))
			asked = true;
		sleeping = int32_t(fence - seqno) < 0;
		return sleeping;
	}

	// tgm_chan_announced: whether the interrupt that wakes a waiter of `seqno` has come
	bool announced(uint32_t seqno) const
	{
		int is = 0;
		const uint32_t end = switch_gpu_jobs_announced_by(&jobs, seqno, &is);
		return is && int32_t(fence - end) >= 0;
	}

	void run(size_t entries = ~size_t(0), bool passes = true)
	{
		while (entries-- && !fifo.empty()) {
			const Entry e = fifo.front();
			fifo.pop_front();
			if (e.seqno)
				fence = e.seqno;
			if (e.interrupts && passes)
				pass();
		}
	}

	void pass()
	{
		++interrupts;
		seen = fence;
		if (sleeping && switch_gpu_waiter_reached(&wanted, fence)) {
			sleeping = false;
			++woken;
		}
	}

	switch_gpu_quiet quiet{};
	switch_gpu_jobs jobs{};
	uint64_t wanted = 0;
	uint32_t submitted, fence, seen;
	std::deque<Entry> fifo;
	bool asked = false, sleeping = false;
	unsigned interrupts = 0, woken = 0, wakeEntries = 0;
};
}  // namespace

TEST(QuietJobs, AQuietJobNobodyWaitsForRaisesNoInterrupt)
{
	Channel c(100);
	c.submit(4, true);
	c.submit(1, true);
	c.run();
	EXPECT_EQ(c.fence, 105u);
	EXPECT_EQ(c.interrupts, 0u);
	EXPECT_FALSE(c.announced(105));
}

TEST(QuietJobs, AWaiterArrivingAfterAQuietJobIsWokenExactlyOnce)
{
	Channel c(100);
	const uint32_t seqno = c.submit(3, true);
	ASSERT_TRUE(c.wait(seqno - 1));
	EXPECT_TRUE(c.asked);
	c.ownerWake();
	EXPECT_EQ(c.wakeEntries, 1u);
	EXPECT_FALSE(c.announced(seqno - 1));
	c.run(1);
	EXPECT_TRUE(c.announced(seqno - 1))
		<< "the job ended and its wake entry is the next one: the wait allows for it";
	EXPECT_EQ(c.woken, 0u);
	c.run();
	EXPECT_EQ(c.interrupts, 1u);
	EXPECT_EQ(c.woken, 1u);
	EXPECT_TRUE(c.announced(seqno - 1));
	c.ownerWake();
	EXPECT_EQ(c.wakeEntries, 1u) << "one entry per ask";
}

// A waiter that asks before the job's fence decides makes that job interrupt: no wake entry.
TEST(QuietJobs, AnAskBeforeTheFenceMakesTheJobInterrupt)
{
	Channel c(7);
	c.submit(1, false);
	ASSERT_TRUE(c.wait(9));
	EXPECT_TRUE(c.asked);
	c.submit(2, true);
	c.ownerWake();
	EXPECT_EQ(c.wakeEntries, 0u);
	c.run();
	EXPECT_EQ(c.interrupts, 2u);
	EXPECT_EQ(c.woken, 1u);
}

// The ask lands after the fence read the asks and before the kick: the waiter saw nothing announced, so the owner's
// call (behind the submission, under its order) pushes the entry.
TEST(QuietJobs, AnAskBetweenTheFenceAndItsKickGetsAWakeEntry)
{
	Channel c(50);
	const int announce = switch_gpu_quiet_announces(&c.quiet, 1);
	ASSERT_FALSE(announce);
	ASSERT_TRUE(c.wait(51));
	EXPECT_TRUE(c.asked);
	c.kick(1, announce);
	c.ownerWake();
	c.run();
	EXPECT_EQ(c.wakeEntries, 1u);
	EXPECT_EQ(c.interrupts, 1u);
	EXPECT_EQ(c.woken, 1u);
}

// The newest job ended and another channel's interrupt ran this channel's pass (its event stays armed) before the
// owner's call: that pass woke the waiter, and no entry is pushed.
TEST(QuietJobs, AJobThePassSawEndNeedsNoEntry)
{
	Channel c(10);
	const uint32_t seqno = c.submit(1, true);
	ASSERT_TRUE(c.wait(seqno));
	c.run();
	c.pass();
	EXPECT_EQ(c.woken, 1u);
	c.ownerWake();
	EXPECT_EQ(c.wakeEntries, 0u);
	EXPECT_TRUE(switch_gpu_quiet_need(&c.quiet, seqno) == 0);
}

// WaitFence's lost-wake check is keyed to the interrupt that wakes it: a waiter whose seqno passed while its entry is
// still queued behind a later job is not unwoken; once that entry ran with no pass, it is.
TEST(QuietJobs, ALostWakeIsAnnouncedOnlyOnceItsInterruptWasDue)
{
	Channel c(0);
	const uint32_t first = c.submit(1, true);
	c.submit(1, true);
	ASSERT_TRUE(c.wait(first));
	c.ownerWake();
	c.run(1, false);
	EXPECT_EQ(c.fence, first);
	EXPECT_FALSE(c.announced(first));
	c.run(~size_t(0), false);
	EXPECT_TRUE(c.announced(first));
	EXPECT_EQ(c.woken, 0u);
}

// The idle clock and teardown ask as a waiter does: with nothing more submitted, the newest job's end interrupts once.
TEST(QuietJobs, ASettleAsksForTheNewestJobOnce)
{
	Channel c(3);
	c.submit(2, true);
	c.submit(1, true);
	EXPECT_EQ(switch_gpu_quiet_need(&c.quiet, c.submitted), 1);
	c.ownerWake();
	EXPECT_EQ(switch_gpu_quiet_need(&c.quiet, c.submitted), 0);
	c.run();
	EXPECT_EQ(c.interrupts, 1u);
	EXPECT_EQ(c.seen, 6u);
}

// Seqnos wrap with the fence word.
TEST(QuietJobs, AsksWrapWithTheFence)
{
	Channel c(0xfffffffeu);
	const uint32_t seqno = c.submit(3, true);
	EXPECT_EQ(seqno, 1u);
	ASSERT_TRUE(c.wait(0xffffffffu));
	c.ownerWake();
	c.run();
	EXPECT_EQ(c.woken, 1u);
	EXPECT_TRUE(c.announced(0u));
}

// A frame's jobs on one channel (the compositor's command buffers, a command buffer split for its push count, its
// event jobs): every job but the frame's last quiet. Before: one interrupt per job; after: one per frame, and the
// completion thread, which sleeps on the frame's last job, wakes once.
TEST(QuietJobs, AFrameOfSeveralJobsInterruptsOnce)
{
	for (const bool quiet : {false, true}) {
		Channel c(1000);
		const uint32_t jobs[] = {9, 1, 6, 1};
		uint32_t last = 0;
		for (size_t i = 0; i < 4; ++i)
			last = c.submit(jobs[i], quiet && i + 1 < 4);
		ASSERT_TRUE(c.wait(last));
		c.ownerWake();
		c.run();
		EXPECT_EQ(c.interrupts, quiet ? 1u : 4u);
		EXPECT_EQ(c.woken, 1u);
		EXPECT_EQ(c.wakeEntries, 0u);
	}
}
