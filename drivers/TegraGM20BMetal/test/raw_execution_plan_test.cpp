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

raw::ExecutionAttribution routeOf(const std::vector<raw::ExecutionRange> &ranges)
{
    raw::ExecutionAttribution route;
    for (const auto &range : ranges)
        route.add(range.kind == ExecutionKind::native, false);
    return route;
}

// Three render encoders recorded raw share one submission; the middle one's draw is refused while it is encoded
TEST(ExecutionPlan, ARefusedEncoderAloneGoesToCompatibilityAndItsNeighboursStayNative)
{
    raw::ExecutionPlan plan;
    plan.append(0, 5, ExecutionKind::native);
    plan.append(5, 9, ExecutionKind::native);
    plan.append(9, 14, ExecutionKind::native);
    ASSERT_EQ(plan.ranges().size(), 1u);
    const auto parts = raw::isolate(plan.ranges()[0], 5, 9);
    ASSERT_EQ(parts.size(), 3u);
    EXPECT_EQ(parts[0].from, 0u);
    EXPECT_EQ(parts[0].to, 5u);
    EXPECT_EQ(parts[0].kind, ExecutionKind::native);
    EXPECT_EQ(parts[1].from, 5u);
    EXPECT_EQ(parts[1].to, 9u);
    EXPECT_EQ(parts[1].kind, ExecutionKind::compatibility);
    EXPECT_EQ(parts[2].from, 9u);
    EXPECT_EQ(parts[2].to, 14u);
    EXPECT_EQ(parts[2].kind, ExecutionKind::native);
    const auto route = routeOf(parts);
    EXPECT_EQ(route.native, 2u);
    EXPECT_EQ(route.compatibility, 1u);
    EXPECT_TRUE(route.mixed());
}

TEST(ExecutionPlan, ARefusedFirstOrLastEncoderLeavesOneNativeSide)
{
    const raw::ExecutionRange range{0, 14, ExecutionKind::native};
    const auto first = raw::isolate(range, 0, 5);
    ASSERT_EQ(first.size(), 2u);
    EXPECT_EQ(first[0].kind, ExecutionKind::compatibility);
    EXPECT_EQ(first[0].to, 5u);
    EXPECT_EQ(first[1].kind, ExecutionKind::native);
    EXPECT_EQ(first[1].from, 5u);
    EXPECT_EQ(first[1].to, 14u);
    const auto last = raw::isolate(range, 9, 14);
    ASSERT_EQ(last.size(), 2u);
    EXPECT_EQ(last[0].kind, ExecutionKind::native);
    EXPECT_EQ(last[0].to, 9u);
    EXPECT_EQ(last[1].kind, ExecutionKind::compatibility);
    EXPECT_EQ(last[1].from, 9u);
}

TEST(ExecutionPlan, ARefusalNoEncoderOwnsSendsTheWholeRangeToCompatibility)
{
    const raw::ExecutionRange range{3, 14, ExecutionKind::native};
    for (const auto &[from, to] : std::vector<std::pair<size_t, size_t>>{{14, 14}, {0, 5}, {9, 20}, {6, 6}}) {
        const auto parts = raw::isolate(range, from, to);
        ASSERT_EQ(parts.size(), 1u);
        EXPECT_EQ(parts[0].from, 3u);
        EXPECT_EQ(parts[0].to, 14u);
        EXPECT_EQ(parts[0].kind, ExecutionKind::compatibility);
        EXPECT_TRUE(routeOf(parts).onlyCompatibility());
    }
}

// A refused range that held one encoder is replaced by that encoder alone: nothing native is left to split
TEST(ExecutionPlan, ARefusedSingleEncoderRangeIsOneCompatibilityRange)
{
    const auto parts = raw::isolate({5, 9, ExecutionKind::native}, 5, 9);
    ASSERT_EQ(parts.size(), 1u);
    EXPECT_EQ(parts[0].kind, ExecutionKind::compatibility);
    EXPECT_EQ(parts[0].from, 5u);
    EXPECT_EQ(parts[0].to, 9u);
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
        queue.reserve(job, job);
        queue.commit(job, job);
    }
    std::vector<int> submitted;
    for (unsigned step = 0; !queue.empty() && step < 20; ++step) {
        const int job = queue.ready([&](int id) {
            const auto kind = plans[id]->ranges()[cursors[id]].kind;
            if (kind == ExecutionKind::wait)
                return signals[id == 1 ? 2 : 1];
            if (kind == ExecutionKind::signal)
                return pending[id] == 0;
            return true;
        });
        if (!job) {
            // Fence completion is independent of selection. No blocked queue owns the submission worker.
            pending = {};
            continue;
        }
        const auto kind = plans[job]->ranges()[cursors[job]].kind;
        if (kind == ExecutionKind::signal)
            signals[job] = true;
        else if (kind != ExecutionKind::wait) {
            ++pending[job];
            submitted.push_back(job);
        }
        if (++cursors[job] == plans[job]->ranges().size())
            ASSERT_TRUE(queue.retire(job, job));
        else
            queue.yield(job);
    }
    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(submitted, (std::vector<int>{1, 2, 1}));
    EXPECT_TRUE(signals[1]);
    EXPECT_TRUE(signals[2]);
}

TEST(SubmitQueue, PartialSubmissionYieldsWithoutReleasingItsReservation)
{
    raw::SubmitQueue<int, int> queue;
    for (int job : {11, 12, 21}) {
        queue.reserve(job / 10, job);
        queue.commit(job / 10, job);
    }
    queue.yield(1);
    EXPECT_EQ(queue.ready([](int) {
        return true;
    }),
              21);
    EXPECT_FALSE(queue.retire(1, 12));
    std::vector<int> heads;
    queue.heads(heads);
    EXPECT_EQ(heads, (std::vector<int>{21, 11}));
    EXPECT_EQ(queue.size(), 3u);
}
}  // namespace
