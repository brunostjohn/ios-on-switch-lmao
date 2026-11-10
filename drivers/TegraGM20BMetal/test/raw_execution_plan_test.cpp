// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include "../raw/runtime/raw_execution_plan.h"
#include "../raw/runtime/raw_submit_queue.h"
#include <array>
#include <map>

namespace {
using raw::ExecutionKind;

TEST(ExecutionAttribution, NativeRangesStayNativeAcrossEvents)
{
    raw::ExecutionAttribution route;
    route.add(true, false);
    route.add(false, true);
    route.add(true, false);
    EXPECT_EQ(route.native, 2u);
    EXPECT_EQ(route.compatibility, 0u);
    EXPECT_FALSE(route.mixed());
    EXPECT_FALSE(route.onlyCompatibility());
}

TEST(ExecutionAttribution, CompatibilityRangesStayCompatibilityAcrossEvents)
{
    raw::ExecutionAttribution route;
    route.add(false, false);
    route.add(false, true);
    route.add(false, false);
    EXPECT_EQ(route.native, 0u);
    EXPECT_EQ(route.compatibility, 2u);
    EXPECT_FALSE(route.mixed());
    EXPECT_TRUE(route.onlyCompatibility());
    route.add(true, false);
    EXPECT_TRUE(route.mixed());
    EXPECT_FALSE(route.onlyCompatibility());
}

TEST(ExecutionAttribution, EmptyAndEventOnlyHaveNoCompatibilityWork)
{
    raw::ExecutionAttribution route;
    for (unsigned count = 0; count != 3; ++count) {
        EXPECT_EQ(route.native, 0u);
        EXPECT_EQ(route.compatibility, 0u);
        EXPECT_FALSE(route.mixed());
        EXPECT_FALSE(route.onlyCompatibility());
        route.add(false, true);
    }
}

TEST(ExecutionPlan, AdjacentCompatibleEncodersShareOneSubmission)
{
    raw::ExecutionPlan plan;
    plan.append(0, 8, ExecutionKind::native);
    plan.append(8, 12, ExecutionKind::native);
    plan.append(12, 18, ExecutionKind::compatibility);
    plan.append(18, 24, ExecutionKind::native);
    const auto &ranges = plan.ranges();
    ASSERT_EQ(ranges.size(), 3u);
    EXPECT_EQ(ranges[0].from, 0u);
    EXPECT_EQ(ranges[0].to, 12u);
    EXPECT_EQ(ranges[1].kind, ExecutionKind::compatibility);
    EXPECT_EQ(ranges[2].from, 18u);
}

TEST(ExecutionPlan, EventsNeverMergeWithWorkOrEachOther)
{
    raw::ExecutionPlan plan;
    plan.append(0, 4, ExecutionKind::native);
    plan.append(4, 5, ExecutionKind::signal);
    plan.append(5, 6, ExecutionKind::wait);
    plan.append(6, 10, ExecutionKind::native);
    plan.append(10, 11, ExecutionKind::signal);
    ASSERT_EQ(plan.ranges().size(), 5u);
    for (size_t i = 1; i < plan.ranges().size(); ++i)
        EXPECT_EQ(plan.ranges()[i - 1].to, plan.ranges()[i].from);
}

TEST(ExecutionPlan, EmptyAndEventOnlyCommandBuffersKeepExactPositions)
{
    raw::ExecutionPlan empty;
    empty.append(0, 0, ExecutionKind::native);
    EXPECT_TRUE(empty.ranges().empty());
    empty.append(0, 1, ExecutionKind::signal);
    empty.append(1, 2, ExecutionKind::signal);
    ASSERT_EQ(empty.ranges().size(), 2u);
    EXPECT_EQ(empty.ranges()[1].from, 1u);
}

TEST(ExecutionPlan, ConsecutiveNativeWaitsAvoidCpuRetirement)
{
    raw::ExecutionPlan plan;
    plan.append(0, 1, ExecutionKind::wait);
    plan.append(1, 2, ExecutionKind::wait);
    plan.append(2, 5, ExecutionKind::native);
    plan.append(5, 6, ExecutionKind::wait);
    plan.append(6, 7, ExecutionKind::signal);
    plan.append(7, 8, ExecutionKind::wait);
    plan.append(8, 10, ExecutionKind::compatibility);
    plan.append(10, 11, ExecutionKind::wait);
    const auto &ranges = plan.ranges();
    ASSERT_EQ(ranges.size(), 8);
    EXPECT_TRUE(ranges[0].gpuWait);
    EXPECT_TRUE(ranges[1].gpuWait);
    for (size_t at = 2; at < ranges.size(); ++at)
        EXPECT_FALSE(ranges[at].gpuWait);
}

// Regression journey: A publishes E then waits F; B (queued later) waits E then publishes F. Preflighting all of A's
// waits deadlocked for ten seconds. Execute the real range plan through the real per-queue reservation/yield machinery.
TEST(ExecutionPlan, InteriorWaitDoesNotPreventItsOwnEarlierSignalFromReleasingTheProducer)
{
    raw::ExecutionPlan a, b;
    a.append(0, 4, ExecutionKind::native);
    a.append(4, 5, ExecutionKind::signal);
    a.append(5, 6, ExecutionKind::wait);
    a.append(6, 9, ExecutionKind::native);
    b.append(0, 1, ExecutionKind::wait);
    b.append(1, 4, ExecutionKind::compatibility);
    b.append(4, 5, ExecutionKind::signal);
    std::array<const raw::ExecutionPlan *, 3> plans{{nullptr, &a, &b}};
    std::array<size_t, 3> cursors{};
    std::array<unsigned, 3> pending{};
    std::map<unsigned, bool> signals;
    raw::SubmitQueue<int, int> queue;
    for (int job : {1, 2}) {
        const auto reservation = queue.reserve(job, job);
        queue.commit(job, job, reservation);
    }
    std::vector<int> submitted;
    for (unsigned step = 0; queue.size() && step < 40; ++step) {
        const auto ticket = queue.next();
        if (!ticket) {
            pending = {};
            queue.wake(1);
            queue.wake(2);
            continue;
        }
        const int job = ticket.value;
        const auto kind = plans[job]->ranges()[cursors[job]].kind;
        if ((kind == ExecutionKind::wait && !signals[job == 1 ? 2 : 1]) ||
            (kind == ExecutionKind::signal && pending[job])) {
            queue.park(ticket);
            continue;
        }
        if (kind == ExecutionKind::signal) {
            signals[job] = true;
            queue.wake(job == 1 ? 2 : 1);
        } else if (kind != ExecutionKind::wait) {
            ++pending[job];
            submitted.push_back(job);
        }
        if (++cursors[job] == plans[job]->ranges().size())
            ASSERT_TRUE(queue.retire(ticket));
        else
            queue.yield(ticket);
    }
    EXPECT_EQ(queue.size(), 0u);
    EXPECT_EQ(submitted, (std::vector<int>{1, 2, 1}));
    EXPECT_TRUE(signals[1]);
    EXPECT_TRUE(signals[2]);
}

TEST(SubmitQueue, PartialSubmissionYieldsWithoutReleasingItsReservation)
{
    raw::SubmitQueue<int, int> queue;
    for (int job : {11, 12, 21}) {
        const auto reservation = queue.reserve(job / 10, job);
        queue.commit(job / 10, job, reservation);
    }
    auto first = queue.next();
    ASSERT_EQ(first.value, 11);
    queue.yield(first);
    auto second = queue.next();
    ASSERT_EQ(second.value, 21);
    auto resumed = queue.next();
    ASSERT_EQ(resumed.value, 11);
    EXPECT_FALSE(queue.next());
    EXPECT_EQ(queue.size(), 3u);
    queue.retire(resumed);
    auto next = queue.next();
    ASSERT_EQ(next.value, 12);
    EXPECT_FALSE(queue.retire(first));
}
}  // namespace
