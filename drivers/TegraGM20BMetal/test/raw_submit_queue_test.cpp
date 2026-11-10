// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include "../raw/runtime/raw_submit_queue.h"

namespace {

TEST(SubmitQueue, AnotherQueuesProducerBypassesBlockedConsumer)
{
    raw::SubmitQueue<int, int> q;
    q.reserve(1, 10);
    q.reserve(1, 11);
    q.reserve(2, 20);
    q.commit(1, 10);
    q.commit(1, 11);
    q.commit(2, 20);
    EXPECT_EQ(q.ready([](int job) {
        return job != 10;
    }),
              20);
    EXPECT_TRUE(q.retire(2, 20));
    EXPECT_EQ(q.ready([](int) {
        return true;
    }),
              10);
    EXPECT_FALSE(q.retire(1, 11));
    EXPECT_TRUE(q.retire(1, 10));
    EXPECT_EQ(q.ready([](int) {
        return true;
    }),
              11);
}

TEST(SubmitQueue, EnqueueReservesOrderBeforeCommit)
{
    raw::SubmitQueue<int, int> q;
    q.reserve(1, 1);
    q.reserve(1, 2);
    q.commit(1, 2);
    EXPECT_EQ(q.ready([](int) {
        return true;
    }),
              0);
    q.commit(1, 1);
    EXPECT_EQ(q.ready([](int) {
        return true;
    }),
              1);
    q.retire(1, 1);
    EXPECT_EQ(q.ready([](int) {
        return true;
    }),
              2);
}

TEST(SubmitQueue, ReusesDrainedQueuesWithoutLosingReservations)
{
    raw::SubmitQueue<int, int> q;
    q.reserve(1, 10);
    q.commit(1, 10);
    ASSERT_TRUE(q.retire(1, 10));
    EXPECT_FALSE(q.retire(1, 10));
    EXPECT_EQ(q.ready([](int) {
        return true;
    }),
              0);
    q.reserve(2, 20);
    q.reserve(1, 11);
    q.commit(1, 11);
    EXPECT_EQ(q.ready([](int) {
        return true;
    }),
              11);
    q.commit(2, 20);
    EXPECT_EQ(q.ready([](int) {
        return true;
    }),
              20);
    ASSERT_TRUE(q.retire(2, 20));
    ASSERT_TRUE(q.retire(1, 11));
    EXPECT_TRUE(q.empty());
}

TEST(SubmitQueue, QueuesTakeTurnsAndBlockedHeadsKeepTheirPlace)
{
    raw::SubmitQueue<int, int> q;
    for (int i : {1, 2, 3}) {
        q.reserve(1, i);
        q.commit(1, i);
    }
    q.reserve(2, 4);
    q.commit(2, 4);
    q.retire(1, 1);
    EXPECT_EQ(q.ready([](int) {
        return true;
    }),
              4);
    EXPECT_EQ(q.ready([](int i) {
        return i != 4;
    }),
              2);
    EXPECT_EQ(q.size(), 3u);
    q.retire(2, 4);
    q.retire(1, 2);
    q.retire(1, 3);
    EXPECT_TRUE(q.empty());
}

TEST(EventDeadline, UnspecifiedTimeoutNeverManufacturesAnEvent)
{
    using namespace std::chrono_literals;
    const raw::EventDeadline::Time start{};
    raw::EventDeadline wait(start, 0);
    EXPECT_EQ(wait.state(false, start + 9s), raw::EventReadiness::blocked);
    EXPECT_EQ(wait.state(false, start + 10s), raw::EventReadiness::expired);
    EXPECT_EQ(wait.state(true, start + 10s), raw::EventReadiness::ready);
}

TEST(EventDeadline, ExplicitTimeoutRetainsItsRequestedDuration)
{
    using namespace std::chrono_literals;
    const raw::EventDeadline::Time start{};
    raw::EventDeadline wait(start, 30000);
    EXPECT_EQ(wait.state(false, start + 10s), raw::EventReadiness::blocked);
    EXPECT_EQ(wait.state(false, start + 30s), raw::EventReadiness::ready);
}

}  // namespace
