// SPDX-License-Identifier: MIT
// A present or a wait parked on a writer promise nobody keeps, from its first look to the panel: the present
// deadline's looks and re-arms (gpu_present_deadline.c) and a wait's hang checks (tgm_hang_check_interval), the hang
// rule's strikes (tgm_hang_confirm, tgm_hang_blame), its raise (switch_gpu_promise_waits_release) with gpu_promise.c's
// step handling, the PROMISE token's status (SwitchGpuWriterPromise.cpp) and the display's handling of
// TEGRA_SCANOUT_SOURCE_LOST (dropLostFrame keeps the frame on the panel). A real hang, not on a promise, is the
// control.
#include <gtest/gtest.h>
#include <string>
#include <vector>
#include "drivers/TegraGM20B/SwitchGpuWriterPromise.h"
#include "drivers/TegraGM20B/TegraGM20BRecover.h"

namespace
{
constexpr uint64_t kMs = 1000 * 1000;
constexpr uint64_t kPresentNs = 1000 * kMs;  // TGM_PRESENT_NS: the deadline, and its re-arm after a look
constexpr uint64_t kHangNs = 2000 * kMs;     // TGM_HANG_NS: a wait's hang check period
constexpr uint32_t kOffset = 2052, kValue = 9;
constexpr int kSourceLost = -5;

volatile uint32_t pool[1024];

volatile uint32_t *word(uint32_t offset)
{
	return offset / 4 < 1024 ? &pool[offset / 4] : nullptr;
}

struct Present {
	switch_gpu_promise_waits waits = {};
	switch_gpu_promise_abandoned abandoned = {};
	uint32_t strikes = 0, seen = 0, completed = 40;
	std::vector<std::string> lines;
	bool makerReleasesAtRaise = false;
	bool producerHung = false;    // the control: the present waits on a client producer stuck on its own work
	bool producerParked = false;  // the present waits on a client producer whose job acquires the promise
	int shown = 1;                // the frame on the panel

	// gpu_promise.c promise_released
	static void released(uint32_t offset, uint32_t value, int step, void *arg)
	{
		auto *p = static_cast<Present *>(arg);
		if (step == SWITCH_GPU_PROMISE_RAISING) {
			switch_gpu_promise_abandoned_add(&p->abandoned, offset, value);
			if (p->makerReleasesAtRaise)
				pool[offset / 4] = value;
			return;
		}
		if (step == SWITCH_GPU_PROMISE_KEPT_FIRST) {
			switch_gpu_promise_abandoned_drop(&p->abandoned, offset, value);
			return;
		}
		p->lines.push_back("writer-app's writer promise at fence offset " + std::to_string(offset) +
				   " was not kept: present waited");
	}

	// tgm_hang_recover at the present deadline. On a promise: the present channel parked on the word, which no live
	// channel's fence is (TGM_WAITS_VICTIM), switched out by its acquire (no engine on it). The control: the producer
	// the present waits on, an engine on it, while other channels progress.
	enum tgm_hang look()
	{
		tgm_hang_candidate c = {};
		c.live = 1;
		c.kernel = !producerHung && !producerParked;
		c.victim = !producerHung;
		c.resident = producerHung;
		c.stuck = uint8_t(tgm_hang_confirm(1, completed, &strikes, &seen));
		if (c.stuck && switch_gpu_promise_waits_release(&waits, word, released, this))
			return TGM_HANG_PROGRESSING;
		int blame = 0;
		return tgm_hang_blame(&c, 1, 0, c.kernel, producerHung, &blame);
	}

	// the deadline's next look after one that was not fatal
	uint64_t recheck() const
	{
		return switch_gpu_promise_waits_pending(&waits, word) ? TGM_STUCK_NS : kPresentNs;
	}

	// SwitchGpuWriterPromise.cpp status
	int status() const
	{
		if (switch_gpu_promise_abandoned_has(&abandoned, kOffset, kValue))
			return -5;
		return int32_t(pool[kOffset / 4] - kValue) >= 0 ? 1 : 0;
	}

	// the present behind a producer: the producer's composite reads what the surface holds, the frame before the
	// promised one when the promise was given up on
	int finishComposite()
	{
		if (status() == 0)
			return -1;
		shown = status() == 1 ? 2 : 1;
		return 0;
	}

	// the present's result once its tokens finish; the display flips only a 0 (NativeLegacyDisplay finishGpuCopy)
	int finish(int frame)
	{
		const int result = status() == 1 ? 0 : kSourceLost;
		if (!result)
			shown = frame;
		return result;
	}
};

class Strike : public testing::Test
{
protected:
	Present p;
	uint64_t now = 0;

	uint64_t next = kPresentNs;

	void SetUp() override
	{
		pool[kOffset / 4] = kValue - 1;
	}

	// the present's job acquires the promise (present_copy: its first look at TGM_PROMISE_FIRST_LOOK_NS)
	void parkOnThePromise()
	{
		ASSERT_EQ(switch_gpu_promise_waits_note(&p.waits, word, kOffset, kValue), 0);
		next = TGM_PROMISE_FIRST_LOOK_NS;
	}

	// tgm_chan_check_progress of a wait on the channel (a fence wait, the bridge's): its next check, then the look
	enum tgm_hang waitCheck()
	{
		now += tgm_hang_check_interval(switch_gpu_promise_waits_pending(&p.waits, word), p.strikes, kHangNs);
		return p.look();
	}

	enum tgm_hang deadline()
	{
		now += next;
		const enum tgm_hang verdict = p.look();
		next = p.recheck();
		return verdict;
	}
};

// The first look, once the parked job has been stuck for TGM_STUCK_NS, takes the first strike: the present goes on
// waiting, and a kernel channel that is a victim is never STUCK (no panic). It looks again TGM_STUCK_NS later, as it
// waits on a promise word, and that look (about 1 s after the offer, as a watched present gives up) confirms the
// strike: the promise is marked given up, its word raised, and one line names its writer. The present finishes
// TEGRA_SCANOUT_SOURCE_LOST and the panel keeps the frame before it.
TEST_F(Strike, AnUnkeptPromiseKeepsThePreviousFrameAndNamesItsWriterOnceAtOneSecond)
{
	parkOnThePromise();
	EXPECT_EQ(deadline(), TGM_HANG_PROGRESSING);
	EXPECT_TRUE(p.lines.empty());
	EXPECT_EQ(pool[kOffset / 4], kValue - 1);
	EXPECT_EQ(deadline(), TGM_HANG_PROGRESSING);
	EXPECT_EQ(now, TGM_PROMISE_FIRST_LOOK_NS + TGM_STUCK_NS);
	EXPECT_LE(now, kPresentNs + 10 * kMs);
	ASSERT_EQ(p.lines.size(), 1u);
	EXPECT_NE(p.lines[0].find("writer-app's writer promise"), std::string::npos);
	EXPECT_EQ(pool[kOffset / 4], kValue);
	EXPECT_EQ(p.status(), -5);
	EXPECT_EQ(p.finish(2), kSourceLost);
	EXPECT_EQ(p.shown, 1);
	EXPECT_EQ(next, kPresentNs) << "a raised word is no longer a promise wait";
	EXPECT_EQ(deadline(), TGM_HANG_PROGRESSING);
	EXPECT_EQ(p.lines.size(), 1u) << "a raised word is not raised again";
}

// The present waits for a GM20B producer (the render server's composite) whose own job acquires the unkept promise
// (tgm_present_waits_promise): its first look comes as early, the producer's channel is struck at about 1 s, one line
// names the writer, and the composite goes on with what the surface held, the frame before the promised one.
TEST_F(Strike, APresentBehindAProducerParkedOnAPromiseGivesUpAtOneSecond)
{
	p.producerParked = true;
	parkOnThePromise();
	EXPECT_EQ(deadline(), TGM_HANG_PROGRESSING);
	EXPECT_TRUE(p.lines.empty());
	EXPECT_EQ(deadline(), TGM_HANG_PROGRESSING);
	EXPECT_EQ(now, TGM_PROMISE_FIRST_LOOK_NS + TGM_STUCK_NS);
	ASSERT_EQ(p.lines.size(), 1u);
	EXPECT_EQ(p.finishComposite(), 0);
	EXPECT_EQ(p.shown, 1);
}

// A client's own wait on its job parked on the promise (a Metal blit's fence wait, VIC's bridge) checks as a present
// does: struck at 0.51 s and given up at 1.01 s, one line, not after two TGM_HANG_NS checks.
TEST_F(Strike, AWaitOnAJobParkedOnAPromiseGivesUpAtOneSecond)
{
	p.producerParked = true;
	parkOnThePromise();
	EXPECT_EQ(waitCheck(), TGM_HANG_PROGRESSING);
	EXPECT_EQ(now, TGM_PROMISE_FIRST_LOOK_NS);
	EXPECT_TRUE(p.lines.empty());
	EXPECT_EQ(waitCheck(), TGM_HANG_PROGRESSING);
	EXPECT_EQ(now, TGM_PROMISE_FIRST_LOOK_NS + TGM_STUCK_NS);
	ASSERT_EQ(p.lines.size(), 1u);
	EXPECT_EQ(tgm_hang_check_interval(switch_gpu_promise_waits_pending(&p.waits, word), p.strikes, kHangNs), kHangNs)
		<< "a raised word is no longer a promise wait";
}

// A wait on a client stuck on its own work, no promise: its checks keep TGM_HANG_NS, struck at 2 s and recovered at
// 4 s.
TEST_F(Strike, APlainStuckClientStillTakesTwoChecksTwoSecondsApart)
{
	p.producerHung = true;
	EXPECT_EQ(waitCheck(), TGM_HANG_PROGRESSING);
	EXPECT_EQ(now, kHangNs);
	EXPECT_EQ(waitCheck(), TGM_HANG_RECOVERING);
	EXPECT_EQ(now, 2 * kHangNs);
	EXPECT_TRUE(p.lines.empty());
	EXPECT_EQ(tgm_hang_check_interval(0, 0, kHangNs), kHangNs);
	EXPECT_EQ(tgm_hang_check_interval(0, 1, kHangNs), kHangNs);
}

// A real hang, not on a promise: the producer is struck at the deadline and again a whole deadline later (2 s), then
// recovered, as before.
TEST_F(Strike, ARealHangStillTakesTwoDeadlines)
{
	p.producerHung = true;
	EXPECT_EQ(deadline(), TGM_HANG_PROGRESSING);
	EXPECT_EQ(next, kPresentNs);
	EXPECT_EQ(deadline(), TGM_HANG_RECOVERING);
	EXPECT_EQ(now, 2 * kPresentNs);
	EXPECT_TRUE(p.lines.empty());
}

// The maker's job releases the word between the looks: the present's job goes on and ends, nothing is raised or
// reported, and the frame is shown.
TEST_F(Strike, AMakersReleaseBetweenTheLooksShowsItsFrame)
{
	parkOnThePromise();
	EXPECT_EQ(deadline(), TGM_HANG_PROGRESSING);
	pool[kOffset / 4] = kValue;
	++p.completed;
	EXPECT_EQ(deadline(), TGM_HANG_PROGRESSING);
	EXPECT_TRUE(p.lines.empty());
	EXPECT_EQ(p.status(), 1);
	EXPECT_EQ(p.finish(2), 0);
	EXPECT_EQ(p.shown, 2);
}

// The maker's job releases the word while the hang rule is raising it: its writes are there, so nothing is reported and
// the frame is shown.
TEST_F(Strike, AMakersReleaseThatWinsTheRaceShowsItsFrame)
{
	parkOnThePromise();
	p.makerReleasesAtRaise = true;
	EXPECT_EQ(deadline(), TGM_HANG_PROGRESSING);
	deadline();
	EXPECT_TRUE(p.lines.empty());
	EXPECT_EQ(p.status(), 1);
	EXPECT_EQ(p.finish(2), 0);
	EXPECT_EQ(p.shown, 2);
}

// A present that progresses between looks is never struck.
TEST_F(Strike, AProgressingPresentIsNotStruck)
{
	parkOnThePromise();
	deadline();
	++p.completed;
	deadline();
	EXPECT_TRUE(p.lines.empty());
	EXPECT_EQ(pool[kOffset / 4], kValue - 1);
}
}  // namespace
