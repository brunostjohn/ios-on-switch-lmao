// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <deque>
#include "../SwitchVICQueue.h"

TEST(SwitchVICQueue, ConfigurationsAreNotReusedUntilTheirFenceRetires)
{
    SwitchVICQueue queue;
    ASSERT_TRUE(queue.reset(40));
    for (uint32_t i = 0; i < queue.kSlots; ++i) {
        EXPECT_EQ(queue.tail(), i);
        ASSERT_TRUE(queue.publish({0, 2000000, i, nullptr, nullptr}));
    }
    EXPECT_FALSE(queue.publish({}));
    EXPECT_FALSE(queue.retire(40));
    EXPECT_EQ(queue.count, queue.kSlots);
    EXPECT_TRUE(queue.retire(41));
    EXPECT_EQ(queue.tail(), 0u);
    EXPECT_TRUE(queue.publish({}));
    EXPECT_EQ(queue.jobs[0].fence, 57u);
    EXPECT_EQ(queue.jobs[queue.head].fence, 42u);
}

TEST(SwitchVICQueue, CompletionAndAdmissionSurviveSyncpointWrap)
{
    SwitchVICQueue queue;
    ASSERT_TRUE(queue.reset(0xfffffffe));
    ASSERT_TRUE(queue.publish({}));
    ASSERT_TRUE(queue.publish({}));
    EXPECT_EQ(queue.submitted, 0u);
    EXPECT_FALSE(queue.retire(0xfffffffe));
    EXPECT_TRUE(queue.retire(0xffffffff));
    EXPECT_TRUE(queue.retire(0));
    EXPECT_EQ(queue.completed, 0u);
    EXPECT_EQ(queue.count, 0u);
}

TEST(SwitchVICQueue, ResetCannotForgetLiveConfigurations)
{
    SwitchVICQueue queue;
    ASSERT_TRUE(queue.publish({}));
    EXPECT_FALSE(queue.reset(123));
    EXPECT_EQ(queue.submitted, 1u);
    EXPECT_TRUE(queue.retire(1));
    EXPECT_TRUE(queue.reset(123));
    EXPECT_EQ(queue.completed, 123u);
}

TEST(SwitchVICQueue, PublishedCallbacksStayWithTheirOwnFence)
{
    SwitchVICQueue queue;
    int first = 0, second = 0;
    auto completion = [](void *p, int value) { *static_cast<int *>(p) = value; };
    ASSERT_TRUE(queue.publish({0, 20, 10, completion, &first}));
    ASSERT_TRUE(queue.publish({0, 40, 30, completion, &second}));
    const auto job = queue.jobs[queue.head];
    ASSERT_TRUE(queue.retire(1));
    job.completion(job.context, 7);
    EXPECT_EQ(first, 7);
    EXPECT_EQ(second, 0);
    EXPECT_EQ(queue.jobs[queue.head].context, &second);
    EXPECT_EQ(queue.jobs[queue.head].fence, 2u);
}

TEST(SwitchVICQueue, WholeBatchesWaitUntilAllConfigurationSlotsAreFree)
{
    SwitchVICQueue queue;
    for (uint32_t i = 0; i < 15; ++i)
        ASSERT_TRUE(queue.publish({}));
    EXPECT_TRUE(queue.canPublish(1));
    EXPECT_FALSE(queue.canPublish(0));
    EXPECT_FALSE(queue.canPublish(2));
    EXPECT_FALSE(queue.canPublish(17));
    ASSERT_TRUE(queue.retire(1));
    ASSERT_TRUE(queue.retire(2));
    ASSERT_TRUE(queue.canPublish(3));
    EXPECT_EQ(queue.configurationSlot(0), 15u);
    EXPECT_EQ(queue.configurationSlot(1), 0u);
    EXPECT_EQ(queue.configurationSlot(2), 1u);
    EXPECT_EQ(queue.jobs[queue.head].fence, 3u);
}

TEST(SwitchVICQueue, MixedBatchAdmissionAndRetirementPreserveEveryLiveConfiguration)
{
    SwitchVICQueue queue;
    ASSERT_TRUE(queue.reset(0xffffff00));
    std::deque<uint64_t> expected;
    uint32_t random = 0x213807u;
    uint64_t sequence = 0;
    for (uint32_t iteration = 0; iteration < 100000; ++iteration) {
        random = random * 1664525u + 1013904223u;
        const uint32_t batch = 1 + (random >> 16) % 3;
        if ((random & 1) && queue.canPublish(batch)) {
            uint32_t slots[3];
            for (uint32_t i = 0; i < batch; ++i)
                slots[i] = queue.configurationSlot(i);
            for (uint32_t i = 0; i < batch; ++i) {
                ASSERT_EQ(queue.tail(), slots[i]);
                ASSERT_TRUE(queue.publish({0, 1, ++sequence, nullptr, nullptr}));
                expected.push_back(sequence);
            }
        } else if (queue.count) {
            ASSERT_EQ(queue.jobs[queue.head].submittedUs, expected.front());
            ASSERT_TRUE(queue.retire(queue.jobs[queue.head].fence));
            expected.pop_front();
        }
        ASSERT_EQ(queue.count, expected.size());
        for (uint32_t i = 0; i < queue.count; ++i)
            ASSERT_EQ(queue.jobs[(queue.head + i) % queue.kSlots].submittedUs, expected[i]);
    }
}
