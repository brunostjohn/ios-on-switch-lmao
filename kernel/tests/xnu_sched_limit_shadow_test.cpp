// kernel/switch/sched/limit_shadow.h on the host: Apple's blocking CPU limit replayed beside a lifted one, the blocks
// it would have made and the time run past each budget.
#include <gtest/gtest.h>

#include "sched/limit_shadow.h"

namespace {

constexpr uint64_t kMs = 1000000;

switch_limit_shadow Shadow(uint64_t interval_ms, uint8_t percentage)
{
    switch_limit_shadow s;
    switch_limit_shadow_init(&s, interval_ms * kMs, percentage);
    return s;
}

TEST(XnuSchedLimitShadow, RunningUnderTheBudgetNeverBlocks)
{
    switch_limit_shadow s = Shadow(30, 90);
    for (uint64_t t = 1; t < 30 * 100; t += 30)
        switch_limit_shadow_ran(&s, t * kMs, (t + 20) * kMs);
    EXPECT_EQ(s.blocks, 0u);
    EXPECT_EQ(s.over, 0u);
}

TEST(XnuSchedLimitShadow, AnIntervalPastItsBudgetIsOneBlockAndItsOverrun)
{
    switch_limit_shadow s = Shadow(30, 90);
    switch_limit_shadow_ran(&s, 1 * kMs, 30 * kMs);  // 29 ms of a 30 ms interval: 2 ms past 27
    EXPECT_EQ(s.blocks, 1u);
    EXPECT_EQ(s.over, 2 * kMs);
    switch_limit_shadow_ran(&s, 30 * kMs, 31 * kMs);  // still the first interval (1..31): one block, 1 ms more
    EXPECT_EQ(s.blocks, 1u);
    EXPECT_EQ(s.over, 3 * kMs);
}

TEST(XnuSchedLimitShadow, ARunAcrossIntervalsIsSplitAtEachRefill)
{
    switch_limit_shadow s = Shadow(30, 90);
    switch_limit_shadow_ran(&s, 10 * kMs, 100 * kMs);  // intervals 10-40, 40-70, 70-100, all spinning
    EXPECT_EQ(s.blocks, 3u);
    EXPECT_EQ(s.over, 9 * kMs);
    switch_limit_shadow_ran(&s, 200 * kMs, 205 * kMs);  // after idle intervals: a fresh one, under budget
    EXPECT_EQ(s.blocks, 3u);
    EXPECT_EQ(s.start, 190 * kMs) << "intervals follow on whole from the first run";
}

}  // namespace
