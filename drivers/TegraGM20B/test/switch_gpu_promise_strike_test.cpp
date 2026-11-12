// SPDX-License-Identifier: MIT
// A fence-only present parked on a writer promise nobody keeps, from its deadline to the panel: the hang rule's looks
// (tgm_hang_confirm, tgm_hang_blame), its raise (switch_gpu_promise_waits_release) with gpu_promise.c's step handling,
// the PROMISE token's status (SwitchGpuWriterPromise.cpp) and the display's handling of TEGRA_SCANOUT_SOURCE_LOST
// (dropLostFrame keeps the frame on the panel).
#include <gtest/gtest.h>
#include <string>
#include <vector>
#include "drivers/TegraGM20B/SwitchGpuWriterPromise.h"
#include "drivers/TegraGM20B/TegraGM20BRecover.h"

namespace
{
constexpr uint64_t kMs = 1000 * 1000;
constexpr uint64_t kPresentNs = 1000 * kMs;  // TGM_PRESENT_NS: the deadline, and its re-arm after a look
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
	int shown = 1;  // the frame on the panel

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

	// tgm_hang_recover at the present deadline: the present channel parked on the word, which no live channel's
	// fence is (TGM_WAITS_VICTIM), switched out by its acquire (no engine on it)
	enum tgm_hang look()
	{
		tgm_hang_candidate c = {};
		c.live = 1;
		c.kernel = 1;
		c.victim = 1;
		c.stuck = uint8_t(tgm_hang_confirm(1, completed, &strikes, &seen));
		if (c.stuck && switch_gpu_promise_waits_release(&waits, word, released, this))
			return TGM_HANG_PROGRESSING;
		int blame = 0;
		return tgm_hang_blame(&c, 1, 0, 1, 0, &blame);
	}

	// SwitchGpuWriterPromise.cpp status
	int status() const
	{
		if (switch_gpu_promise_abandoned_has(&abandoned, kOffset, kValue))
			return -5;
		return int32_t(pool[kOffset / 4] - kValue) >= 0 ? 1 : 0;
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

	void SetUp() override
	{
		pool[kOffset / 4] = kValue - 1;
		ASSERT_EQ(switch_gpu_promise_waits_note(&p.waits, word, kOffset, kValue), 0);
	}

	enum tgm_hang deadline()
	{
		now += kPresentNs;
		return p.look();
	}
};

// The first deadline (1 s after the offer) takes the first strike: the present goes on waiting, and a kernel channel
// that is a victim is never STUCK (no panic). The re-armed deadline (2 s) confirms it: the promise is marked given up,
// its word raised, one line names its writer. The present finishes TEGRA_SCANOUT_SOURCE_LOST and the panel keeps the
// frame before it.
TEST_F(Strike, AnUnkeptPromiseKeepsThePreviousFrameAndNamesItsWriterOnce)
{
	EXPECT_EQ(deadline(), TGM_HANG_PROGRESSING);
	EXPECT_TRUE(p.lines.empty());
	EXPECT_EQ(pool[kOffset / 4], kValue - 1);
	EXPECT_EQ(deadline(), TGM_HANG_PROGRESSING);
	EXPECT_EQ(now, 2 * kPresentNs);
	ASSERT_EQ(p.lines.size(), 1u);
	EXPECT_NE(p.lines[0].find("writer-app's writer promise"), std::string::npos);
	EXPECT_EQ(pool[kOffset / 4], kValue);
	EXPECT_EQ(p.status(), -5);
	EXPECT_EQ(p.finish(2), kSourceLost);
	EXPECT_EQ(p.shown, 1);
	EXPECT_EQ(deadline(), TGM_HANG_PROGRESSING);
	EXPECT_EQ(p.lines.size(), 1u) << "a raised word is not raised again";
}

// The maker's job releases the word while the hang rule is raising it: its writes are there, so nothing is reported and
// the frame is shown.
TEST_F(Strike, AMakersReleaseThatWinsTheRaceShowsItsFrame)
{
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
	deadline();
	++p.completed;
	deadline();
	EXPECT_TRUE(p.lines.empty());
	EXPECT_EQ(pool[kOffset / 4], kValue - 1);
}
}  // namespace
