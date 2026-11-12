// SPDX-License-Identifier: MIT
// A writer promise nobody keeps, given up once (SwitchGpuPromiseGiveUp.h), run as the kernel builds it:
// gpu_promise_giveup.c whole (:promise_giveup_source) and gpu_promise.c's GPU-reader park (:promise_note_source),
// under switch_gpu_promise_giveup_stubs.h's clock, call, fence pool and channels. Each GPU reader kind parks the way
// its submission does (tgm_chan_note_promise after the acquires are kicked); every one is given up about a second after
// it parked, with one line, and its reader goes on with the frame before the promised one.
#include <gtest/gtest.h>
#include <string>
#include <vector>
#include "switch_gpu_promise_giveup_stubs.h"
#include "drivers/TegraGM20B/test/promise_giveup_source.h"
#include "drivers/TegraGM20B/test/promise_note_source.h"

namespace
{
constexpr uint64_t kMs = 1000 * 1000;
constexpr uint32_t kOffset = 2052, kValue = 9;
constexpr int kPrevious = 1, kPromised = 2;

struct Kind {
	const char *name;
	const char *owner;  // the reader's channel
	bool present;       // the GPU present's own channel
};

const Kind kKinds[] = {
	{"RawUntimedWaitFence", "Camera", false},   {"WriterWaitIOSurfaceLock", "Photos", false},
	{"FenceOnlyPresent", "present", true},      {"Composite", "backboardd", false},
	{"BridgeVicNvdec", "host1x-bridge", false},
};

class Fixture : public testing::Test
{
protected:
	virtual const char *readerOwner() const
	{
		return "Camera";
	}

	virtual bool readerPresent() const
	{
		return false;
	}

	fake_tgm_chan reader, maker;
	std::vector<std::string> lines;
	uint32_t readerFirst = 0;

	void SetUp() override
	{
		fake::now = 1000 * kMs;
		fake::call = {};
		fake::chans = {&reader, &maker};
		fake::abandoned.clear();
		fake::owner = 1;
		fake::hangLooks = fake::pathAbandoned = fake::directGivenUp = fake::cpuRaised = 0;
		presentReleased = 0;
		std::memset(&giveups, 0, sizeof(giveups));
		giveup_call = nullptr;
		for (auto &word : fake::pool)
			word = 0;
		fake::pool[kOffset / 4] = kValue - 1;
		std::memset(&tgm_fence_pages, 0, sizeof(tgm_fence_pages));
		tgm_fence_pages.made[0][kOffset / 4 - TGM_FENCE_PROMISE_WORD] = kValue;
		setChan(reader, 0x10, readerOwner(), 40);
		setChan(maker, 0x30, "maker-app", 20);
		tgm_present.chan = readerPresent() ? &reader : nullptr;
		interrupts_off::logged = &lines;
		ASSERT_EQ(tgm_promise_giveup_open(), 0);
	}

	void TearDown() override
	{
		interrupts_off::logged = nullptr;
		EXPECT_EQ(interrupts_off::depth, 0);
		EXPECT_EQ(tgm_live_locked, 0);
	}

	static void setChan(fake_tgm_chan &chan, uint64_t id, const char *owner, uint32_t done)
	{
		chan.id = id;
		chan.error = 0;
		chan.fence = done;
		chan.fence_cpu = &chan.fence;
		chan.submitted = done;
		chan.stall_ns = 0;
		chan.strikes = 0;
		chan.recovery = 0;
		chan.asked.clear();
		std::strncpy(chan.owner, owner, sizeof(chan.owner) - 1);
		switch_gpu_jobs_init(&chan.jobs, done);
	}

	// The reader's job, its acquire on the promise kicked: chan_exec parks it, then fences the job
	void readerSubmits()
	{
		readerFirst = reader.submitted + 1;
		const switch_gpu_backing_token promise = {kValue, 0, SWITCH_GPU_ENGINE_PROMISE,
							  kOffset};
		tgm_chan_park_promises(&reader, &promise, 1);
		reader.submitted = readerFirst;
	}

	// The maker's job that keeps the promise, kicked behind `ahead` jobs of its channel still running
	void makerKeeps(uint32_t ahead)
	{
		const uint32_t first = maker.submitted + 1;
		maker.submitted += 1;
		maker.fence = first - 1 - ahead;
		switch_gpu_jobs_kick(&maker.jobs, first, first, fake::now, 1);
		const switch_gpu_backing_token job = {first, uint32_t(maker.id >> 32) + 1,
						      SWITCH_GPU_ENGINE_GM20B, uint32_t(maker.id)};
		tgm_promise_kept(kOffset, kValue, &maker, &job);
	}

	// The keeping job's end: its fence, then its push's release of the word, then its interrupt's pass
	void makerReleases()
	{
		maker.fence = maker.submitted;
		fake::pool[kOffset / 4] = kValue;
		pass();
	}

	// GPU readers parked on the word go on once it reaches the value
	void gpuRuns()
	{
		if (int32_t(fake::pool[kOffset / 4] - kValue) >= 0)
			reader.fence = reader.submitted;
	}

	// The end of a GPU interrupt pass, where nothing may lock: interrupts off as strictly as a filter
	void pass()
	{
		unsigned long flags = 0;
		local_irq_save(flags);
		tgm_promise_pass();
		local_irq_restore(flags);
	}

	// The clock moves to `t`, the call running whenever it is due
	void until(uint64_t t)
	{
		while (fake::call.pending && fake::call.due <= t) {
			fake::now = fake::call.due;
			fake::call.pending = false;
			++fake::call.ran;
			fake::call.fn(nullptr);
			gpuRuns();
		}
		fake::now = t;
	}

	int status() const
	{
		const switch_gpu_backing_token promise = {kValue, 0, SWITCH_GPU_ENGINE_PROMISE,
							  kOffset};
		return switch_gpu_backing_token_status(&promise);
	}

	// What the panel shows once the reader's present of the surface finishes (finishGpuCopy flips only a 0)
	int shown() const
	{
		return status() == 1 ? kPromised : kPrevious;
	}
};

class GiveUp : public Fixture, public testing::WithParamInterface<Kind>
{
protected:
	const char *readerOwner() const override
	{
		return GetParam().owner;
	}

	bool readerPresent() const override
	{
		return GetParam().present;
	}
};

// The reader parks; nobody keeps the promise. A second later the call gives it up: marked abandoned, the word raised
// (the reader's acquire passes, its job completes), one line naming the maker, the reader and how long it waited. The
// raw untimed wait (10 s contract) and every other reader are let go at 1 s, never at a hang check.
TEST_P(GiveUp, IsGivenUpOnceAboutOneSecondAfterThePark)
{
	readerSubmits();
	EXPECT_EQ(fake::call.armed, 1u);
	until(fake::now + 999 * kMs);
	EXPECT_TRUE(lines.empty());
	EXPECT_NE(reader.fence, reader.submitted);
	until(fake::now + 1 * kMs);
	const uint64_t second = fake::now;
	EXPECT_EQ(lines.size(), 1u) << "given up a second after the park";
	until(second + 9000 * kMs);
	ASSERT_EQ(lines.size(), 1u) << "given up within the raw wait's 10 s contract";
	const std::string expect =
		std::string(
			"tegra-gm20b: maker-app's writer promise at fence offset 2052 (value 9) "
			"was not kept: ") +
		GetParam().owner + " waited 1000 ms for it in " +
		(GetParam().present ? "a present job" : "a GPU job");
	EXPECT_EQ(lines[0].substr(0, expect.size()), expect);
	EXPECT_EQ(fake::pool[kOffset / 4], kValue);
	EXPECT_EQ(reader.fence, reader.submitted) << "the reader's acquire passed";
	EXPECT_EQ(status(), -5);
	EXPECT_EQ(shown(), kPrevious);
	EXPECT_EQ(fake::pathAbandoned, 1u);
	EXPECT_EQ(fake::cpuRaised, 1u);
	EXPECT_FALSE(fake::call.pending) << "nothing left to wait on";
	until(second + 20000 * kMs);
	EXPECT_EQ(lines.size(), 1u);
	EXPECT_EQ(fake::call.ran, 1u);
}

// An ordinary kept promise: armed once at the park, cancelled by the keeping job's interrupt pass, never run.
TEST_P(GiveUp, AKeptPromiseCancelsTheCallWhichNeverRuns)
{
	readerSubmits();
	makerKeeps(0);
	EXPECT_EQ(maker.asked, std::vector<uint32_t>{maker.submitted}) << "its end is asked for";
	until(fake::now + 300 * kMs);
	makerReleases();
	gpuRuns();
	EXPECT_EQ(fake::call.armed, 1u);
	EXPECT_EQ(fake::call.cancelled, 1u);
	until(fake::now + 5000 * kMs);
	EXPECT_EQ(fake::call.ran, 0u);
	EXPECT_TRUE(lines.empty());
	EXPECT_EQ(shown(), kPromised);
}

// The reader goes away before the second is up (its channel killed): the call drops the promise, untold and unraised.
TEST_P(GiveUp, AReaderThatLeftIsNotWaitedForAndNothingIsGivenUp)
{
	readerSubmits();
	reader.error = 1;
	until(fake::now + 1000 * kMs);
	EXPECT_EQ(fake::call.ran, 1u);
	EXPECT_TRUE(lines.empty());
	EXPECT_EQ(fake::pool[kOffset / 4], kValue - 1);
	EXPECT_EQ(status(), 0) << "a later keep still shows its frame";
	EXPECT_FALSE(fake::call.pending);
}

// The maker's GPU release lands between the mark and the raise: its writes are there, so the mark is taken back and
// nothing is reported.
TEST_P(GiveUp, TheMakersReleaseRacingTheGiveUpShowsItsFrame)
{
	readerSubmits();
	until(fake::now + 999 * kMs);

	struct Race {
		static void release(uint32_t offset, uint32_t value, int step, void *entry)
		{
			if (step == SWITCH_GPU_PROMISE_RAISING)
				fake::pool[offset / 4] = value;
			giveup_stepped(offset, value, step, entry);
		}
	};

	switch_gpu_giveup_ops racing = giveup_ops;
	racing.stepped = Race::release;
	fake::now += 1 * kMs;
	switch_gpu_giveup_run(&giveups, &racing, fake::now);
	EXPECT_TRUE(lines.empty());
	EXPECT_EQ(status(), 1);
	EXPECT_EQ(shown(), kPromised);
	EXPECT_EQ(fake::cpuRaised, 0u);
}

// The maker closed and its fence page went to another connection, cleared: the word is not the promise's any more. It is
// dropped, never raised into the new owner's word, and nothing is told.
TEST_P(GiveUp, AWordWhosePageChangedOwnerIsDroppedNeverRaised)
{
	readerSubmits();
	++fake::owner;
	fake::pool[kOffset / 4] = 0;
	until(fake::now + 1000 * kMs);
	EXPECT_TRUE(lines.empty());
	EXPECT_EQ(fake::pool[kOffset / 4], 0u);
	EXPECT_TRUE(fake::abandoned.empty());
	EXPECT_FALSE(fake::call.pending);
}

// The promised job is submitted behind another job of its channel still running. At the second it is not given up: it
// waits for that job (its interrupt asked for), and from that job's completion the promised job has its own second.
TEST_P(GiveUp, AJobQueuedBehindAnotherIsJudgedFromThatJobsCompletion)
{
	readerSubmits();
	makerKeeps(1);
	until(fake::now + 1000 * kMs);
	EXPECT_TRUE(lines.empty());
	ASSERT_FALSE(maker.asked.empty());
	EXPECT_EQ(maker.asked.back(), maker.submitted - 1)
		<< "the job ahead's interrupt is asked for";
	until(fake::now + 1500 * kMs);
	EXPECT_TRUE(lines.empty());
	maker.fence = maker.submitted - 1;
	const uint64_t ahead_done = fake::now;
	pass();
	EXPECT_TRUE(fake::call.pending);
	EXPECT_EQ(fake::call.due, ahead_done) << "the pass runs the call now";
	until(ahead_done + 999 * kMs);
	EXPECT_TRUE(lines.empty());
	until(ahead_done + 1000 * kMs);
	ASSERT_EQ(lines.size(), 1u);
	EXPECT_NE(lines[0].find("waited 3500 ms"), std::string::npos) << lines[0];
	EXPECT_EQ(shown(), kPrevious);
}

// The job ahead never completes: the hang rule's, not this rule's. The call is that channel's waiter on the hang rule's
// own two strikes; the recovery (here an engine reset that lets the job ahead complete) hands the promised job back,
// judged from its own start.
TEST_P(GiveUp, AStuckJobAheadIsTheHangRulesAndThePromisedJobIsJudgedFromItsOwnStart)
{
	readerSubmits();
	makerKeeps(1);
	maker.recovery = 2;
	until(fake::now + 1000 * kMs);
	const uint64_t deferred = fake::now;
	EXPECT_EQ(fake::hangLooks, 1u);
	until(deferred + 4000 * kMs);
	EXPECT_EQ(maker.fence, maker.submitted - 1) << "recovered at the second strike";
	const uint64_t start = fake::now;
	EXPECT_TRUE(lines.empty());
	until(start + 999 * kMs);
	EXPECT_TRUE(lines.empty());
	until(start + 1000 * kMs);
	ASSERT_EQ(lines.size(), 1u);
	EXPECT_EQ(shown(), kPrevious);
}

// The hang rule's recovery kills the maker's channel: the promised job died with it, and the promise is given up at the
// next look.
TEST_P(GiveUp, AJobAheadWhoseChannelTheHangRuleKillsGivesThePromiseUp)
{
	readerSubmits();
	makerKeeps(1);
	maker.recovery = 1;
	until(fake::now + 1000 * kMs + 6000 * kMs);
	ASSERT_EQ(lines.size(), 1u);
	EXPECT_EQ(maker.error, 1u);
	EXPECT_EQ(shown(), kPrevious);
}

INSTANTIATE_TEST_SUITE_P(Readers, GiveUp, testing::ValuesIn(kKinds),
			 [](const testing::TestParamInfo<Kind> &info) {
				 return std::string(info.param.name);
			 });

// Waiters with no GPU job: each parks itself and leaves itself, at the keep included (the direct present then watches
// the job; a CPU reader then waits on the job's fence).
struct Waiter {
	const char *name;
	const char *waiter, *who;
	int display;
};

const Waiter kWaiters[] = {
	{"DirectPresentWatch", "the display", "a direct present", 1},
	{"CpuLockIOSurface", "SpringBoard", "a CPU lock", 0},
	{"CpuLeaseDisplayCopy", "backboardd", "a CPU copy", 0},
};

class Leaves : public Fixture, public testing::WithParamInterface<Waiter>
{
protected:
	int park()
	{
		return tgm_promise_park(kOffset, kValue, GetParam().waiter, GetParam().who, nullptr,
					GetParam().display);
	}

	void leave()
	{
		tgm_promise_leave(kOffset, kValue, GetParam().display);
	}

	const Waiter &waiter() const
	{
		return GetParam();
	}
};

TEST_P(Leaves, IsGivenUpOnceAboutOneSecondAfterThePark)
{
	ASSERT_EQ(park(), 1);
	until(fake::now + 999 * kMs);
	EXPECT_TRUE(lines.empty());
	until(fake::now + 1 * kMs);
	ASSERT_EQ(lines.size(), 1u);
	const std::string expect =
		std::string(
			"tegra-gm20b: maker-app's writer promise at fence offset 2052 (value 9) "
			"was not kept: ") +
		waiter().waiter + " waited 1000 ms for it in " + waiter().who;
	EXPECT_EQ(lines[0].substr(0, expect.size()), expect);
	EXPECT_EQ(status(), -5);
	EXPECT_EQ(shown(), kPrevious);
	EXPECT_EQ(fake::directGivenUp, unsigned(waiter().display));
	leave();
	until(fake::now + 10000 * kMs);
	EXPECT_EQ(lines.size(), 1u);
}

// Kept: the waiter leaves at the keep (its wait is on the job from then), which cancels the call.
TEST_P(Leaves, AKeptPromiseCancelsTheCallWhichNeverRuns)
{
	ASSERT_EQ(park(), 1);
	makerKeeps(0);
	leave();
	makerReleases();
	until(fake::now + 5000 * kMs);
	EXPECT_EQ(fake::call.armed, 1u);
	EXPECT_EQ(fake::call.cancelled, 1u);
	EXPECT_EQ(fake::call.ran, 0u);
	EXPECT_TRUE(lines.empty());
}

TEST_P(Leaves, TheLastWaiterLeavingCancelsTheCall)
{
	ASSERT_EQ(park(), 1);
	ASSERT_EQ(park(), 1);
	leave();
	EXPECT_EQ(fake::call.cancelled, 0u);
	leave();
	EXPECT_EQ(fake::call.cancelled, 1u);
	until(fake::now + 5000 * kMs);
	EXPECT_EQ(fake::call.ran, 0u);
	EXPECT_TRUE(lines.empty());
	EXPECT_EQ(status(), 0);
}

TEST_P(Leaves, TheMakersReleaseRacingTheGiveUpShowsItsFrame)
{
	ASSERT_EQ(park(), 1);

	struct Race {
		static void release(uint32_t offset, uint32_t value, int step, void *entry)
		{
			if (step == SWITCH_GPU_PROMISE_RAISING)
				fake::pool[offset / 4] = value;
			giveup_stepped(offset, value, step, entry);
		}
	};

	switch_gpu_giveup_ops racing = giveup_ops;
	racing.stepped = Race::release;
	fake::now += 1000 * kMs;
	switch_gpu_giveup_run(&giveups, &racing, fake::now);
	EXPECT_TRUE(lines.empty());
	EXPECT_EQ(status(), 1);
}

TEST_P(Leaves, AWordWhosePageChangedOwnerIsDroppedNeverRaised)
{
	ASSERT_EQ(park(), 1);
	++fake::owner;
	fake::pool[kOffset / 4] = 0;
	until(fake::now + 1000 * kMs);
	EXPECT_TRUE(lines.empty());
	EXPECT_EQ(fake::pool[kOffset / 4], 0u);
	EXPECT_TRUE(fake::abandoned.empty());
}

// The promised job behind another, or stuck behind one: the waiter left at the keep, so the job is the hang rule's (the
// present's deadline from the keep, the CPU wait's own looks), and nothing is given up here.
TEST_P(Leaves, AJobBehindAnotherIsNotThisWaitersPromiseAnyMore)
{
	ASSERT_EQ(park(), 1);
	makerKeeps(1);
	maker.recovery = 2;
	leave();
	until(fake::now + 10000 * kMs);
	EXPECT_EQ(fake::call.ran, 0u);
	EXPECT_TRUE(lines.empty());
	EXPECT_EQ(fake::hangLooks, 0u);
}

// The word moves with no keep the waiter heard of: the pass runs the call, which hands the direct present its frame.
TEST_P(Leaves, AWordMovedUnheardIsHandedToTheDirectPresent)
{
	ASSERT_EQ(park(), 1);
	fake::pool[kOffset / 4] = kValue;
	pass();
	until(fake::now);
	EXPECT_EQ(presentReleased, unsigned(waiter().display));
	EXPECT_TRUE(lines.empty());
	EXPECT_EQ(status(), 1);
}

INSTANTIATE_TEST_SUITE_P(Waiters, Leaves, testing::ValuesIn(kWaiters),
			 [](const testing::TestParamInfo<Waiter> &info) {
				 return std::string(info.param.name);
			 });

class Call : public GiveUp
{
};

// Idle: no reader parked, nothing armed, and the interrupt pass costs a load.
TEST_P(Call, NothingIsArmedAtIdle)
{
	for (int i = 0; i < 100; ++i)
		pass();
	until(fake::now + 10000 * kMs);
	EXPECT_EQ(fake::call.armed + fake::call.immediate + fake::call.cancelled + fake::call.ran,
		  0u);
}

// A promise kept before its reader parks is never waited on.
TEST_P(Call, AWordAlreadyKeptArmsNothing)
{
	fake::pool[kOffset / 4] = kValue;
	readerSubmits();
	EXPECT_EQ(fake::call.armed, 0u);
	EXPECT_EQ(giveups.used, 0u);
}

// The reader parks after keptPromises ran (its token was taken before): the kept job is recalled, so a job queued
// behind another is still judged from its turn.
TEST_P(Call, AReaderParkedAfterTheKeepFindsTheJob)
{
	makerKeeps(1);
	readerSubmits();
	EXPECT_EQ(maker.asked, std::vector<uint32_t>{maker.submitted});
	until(fake::now + 1000 * kMs);
	EXPECT_TRUE(lines.empty()) << "deferred behind the job ahead";
}

// The call takes tgm_live_lock, a sleeping lock: it may never run where interrupts are off (a filter, under a simple
// lock). It runs in thread context, the raise its only window with interrupts off, and its line logged outside it.
TEST_P(Call, TheCallRunsInThreadContextAndOnlyTheRaiseHasInterruptsOff)
{
	readerSubmits();
	fake::now += 1000 * kMs;
	unsigned long flags = 0;
	local_irq_save(flags);
	EXPECT_THROW(giveup_run(nullptr), std::runtime_error);
	local_irq_restore(flags);
	tgm_live_locked = 0;
	interrupts_off::windows.clear();
	interrupts_off::watched = &fake::pool[kOffset / 4];
	EXPECT_NO_THROW(giveup_run(nullptr));
	interrupts_off::watched = nullptr;
	EXPECT_EQ(lines.size(), 1u);
	const std::vector<std::pair<uint32_t, uint32_t>> raise = {{kValue - 1, kValue}};
	std::vector<std::pair<uint32_t, uint32_t>> moved;
	for (const auto &window : interrupts_off::windows)
		if (window.first != window.second)
			moved.push_back(window);
	EXPECT_EQ(moved, raise)
		<< "the word moves inside one window with interrupts off, the raise's";
	EXPECT_GE(interrupts_off::windows.size(), 3u)
		<< "the raise, and the call's arming under its simple lock";
}

INSTANTIATE_TEST_SUITE_P(Raw, Call, testing::Values(kKinds[0]),
			 [](const testing::TestParamInfo<Kind> &info) {
				 return std::string(info.param.name);
			 });
}  // namespace
