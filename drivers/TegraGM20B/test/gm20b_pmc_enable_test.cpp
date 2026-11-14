// TegraGM20BPmcEnable.h with two of PMC_ENABLE's writers on one register: a recovery resetting GR (its disable, then its
// enable) and an engine's first use enabling the copy engine. The second writer's change lands at the first read of
// the register by the other: at once when that read is outside the lock, or, as a second CPU spinning on the lock
// would, when the lock is released.
#include <cstdint>
#include <functional>
#include <vector>
#include <gtest/gtest.h>

#include "drivers/TegraGM20B/TegraGM20BPmcEnable.h"

namespace
{
constexpr uint32_t kGr = 1u << 12, kCe = 1u << 6, kPmu = 1u << 13;

uint32_t gPmcEnable;
bool gHeld;
std::vector<std::function<void()>> gSpinning;
std::function<void()> gSecond;

uint32_t readPmc(void *)
{
	const uint32_t value = gPmcEnable;
	if (gSecond) {
		std::function<void()> second = std::move(gSecond);
		gSecond = nullptr;
		if (gHeld)
			gSpinning.push_back(second);
		else
			second();
	}
	return value;
}

void writePmc(void *, uint32_t value)
{
	gPmcEnable = value;
}

unsigned long lockPmc(void *)
{
	EXPECT_FALSE(gHeld) << "a change that found the lock held ran before its release";
	gHeld = true;
	return 0;
}

void unlockPmc(void *, unsigned long)
{
	gHeld = false;
	std::vector<std::function<void()>> spinning;
	spinning.swap(gSpinning);
	for (const auto &step : spinning)
		step();
}

const tgm_pmc_enable_io kIo = {nullptr, readPmc, writePmc, lockPmc, unlockPmc};

void change(uint32_t mask, uint32_t value)
{
	tgm_pmc_enable_change(&kIo, mask, value);
}

TEST(Gm20bPmcEnable, ARecoveryResetKeepsTheEnableAFirstUseMadeInsideIt)
{
	gPmcEnable = kGr | kPmu;
	gSecond = [] {
		change(kCe, kCe);
	};
	change(kGr, 0);
	change(kGr, kGr);
	EXPECT_EQ(gPmcEnable, kGr | kPmu | kCe)
		<< "the copy engine's enable was lost: it stays in reset";
}

TEST(Gm20bPmcEnable, AFirstUseKeepsTheResetARecoveryMadeInsideIt)
{
	gPmcEnable = kGr | kPmu;
	gSecond = [] {
		change(kGr, 0);
	};
	change(kCe, kCe);
	EXPECT_EQ(gPmcEnable, kPmu | kCe)
		<< "GR came out of reset under a recovery that holds it there";
}
}  // namespace
