// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include "../SwitchRouteBackoff.h"

namespace {

TEST(SwitchRouteBackoff, AFailureHoldsTheRouteOffForItsCleanRunThenRetriesIt)
{
    SwitchRouteBackoff<4, 32, 100> b;
    EXPECT_TRUE(b.on());
    b.fail();
    for (int i = 0; i < 3; ++i) {
        EXPECT_FALSE(b.on());
        EXPECT_FALSE(b.clean());
    }
    EXPECT_TRUE(b.clean());
    EXPECT_TRUE(b.on());
    EXPECT_EQ(b.failures, 1u);
    EXPECT_EQ(b.retries, 1u);
}

TEST(SwitchRouteBackoff, ADirtyEventWhileOffStartsTheRunOver)
{
    SwitchRouteBackoff<4, 32, 100> b;
    b.fail();
    b.clean();
    b.clean();
    b.dirty();
    EXPECT_EQ(b.left, 4u);
    b.dirty();
    EXPECT_EQ(b.left, 4u);
}

TEST(SwitchRouteBackoff, RepeatedFailuresDoubleTheRunUpToItsCap)
{
    SwitchRouteBackoff<4, 32, 100> b;
    const uint32_t runs[] = {4, 8, 16, 32, 32, 32};
    for (uint32_t want : runs) {
        b.fail();
        EXPECT_EQ(b.run, want);
        while (!b.clean()) {
        }
    }
    EXPECT_EQ(b.retries, 6u);
}

TEST(SwitchRouteBackoff, ARouteUpForItsForgiveRunStartsFromTheFirstRunAgain)
{
    SwitchRouteBackoff<4, 32, 10> b;
    b.fail();
    while (!b.clean()) {
    }
    b.fail();
    EXPECT_EQ(b.run, 8u);
    while (!b.clean()) {
    }
    for (int i = 0; i < 10; ++i)
        b.clean();
    b.fail();
    EXPECT_EQ(b.run, 4u);
}

TEST(SwitchRouteBackoff, CleanEventsWhileOnDoNothingUntilAFailure)
{
    SwitchRouteBackoff<4, 32, 10> b;
    for (int i = 0; i < 100; ++i)
        EXPECT_FALSE(b.clean());
    EXPECT_EQ(b.retries, 0u);
    EXPECT_TRUE(b.on());
}

}  // namespace
