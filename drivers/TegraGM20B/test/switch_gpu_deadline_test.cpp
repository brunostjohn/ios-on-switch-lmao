// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <cstdint>

namespace
{
struct {
	int on, low, dropping;
	uint64_t service_until, last_busy;
	void *raise_w;
} idle;

uint64_t now;
unsigned scheduled;

uint64_t nvxnu_nanotime()
{
	return now;
}

void nvxnu_work_schedule(void *)
{
	++scheduled;
}

#define READ_ONCE(value) (value)
#include "drivers/TegraGM20B/SwitchGpuIdleDeadline.h"

class GpuDeadline : public ::testing::Test
{
protected:
	void SetUp() override
	{
		idle = {};
		idle.on = 1;
		now = 1000000000;
		scheduled = 0;
	}
};

TEST_F(GpuDeadline, ClampsServiceWindowAndPreservesExistingHold)
{
	tegra_gm20b_perf_deadline_ns(now + 1000000000);
	EXPECT_EQ(idle.service_until, now + 20000000);
	tegra_gm20b_perf_deadline_ns(now + 5000000);
	EXPECT_EQ(idle.service_until, now + 20000000);
	EXPECT_EQ(idle.last_busy, now);
	EXPECT_EQ(scheduled, 0u);
}

TEST_F(GpuDeadline, IdleAndRacingDropScheduleOnlyExistingRaiseWork)
{
	idle.low = 1;
	tegra_gm20b_perf_deadline_ns(now + 1000);
	EXPECT_EQ(scheduled, 1u);
	idle.low = 0;
	idle.dropping = 1;
	tegra_gm20b_perf_deadline_ns(now + 2000);
	EXPECT_EQ(scheduled, 2u);
}

TEST_F(GpuDeadline, ExpiredAndDisabledRequestsDoNothing)
{
	tegra_gm20b_perf_deadline_ns(now);
	idle.on = 0;
	tegra_gm20b_perf_deadline_ns(now + 1000);
	EXPECT_EQ(idle.service_until, 0u);
	EXPECT_EQ(idle.last_busy, 0u);
	EXPECT_EQ(scheduled, 0u);
}
}
