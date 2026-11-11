// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include "../raw/runtime/raw_event_job.h"

namespace {
using raw::EventRangeView;

EventRangeView encoder()
{
    return {};
}

EventRangeView signal(bool leasable = true, bool carried = false)
{
    return {true, true, leasable, carried};
}

EventRangeView wait()
{
    return {true, false, true, false};
}

TEST(EventJob, ASignalAfterAnM2vRangeCarriesTheKernelSignalsRightAfterIt)
{
    const std::vector<EventRangeView> ranges = {encoder(), signal(), signal(), signal(false), signal()};
    EXPECT_EQ(raw::signalJobRanges(ranges, 1), (std::vector<size_t>{1, 2}));
}

TEST(EventJob, ACommandBufferOfSignalsAloneIsOneJob)
{
    EXPECT_EQ(raw::signalJobRanges({signal()}, 0), (std::vector<size_t>{0}));
    EXPECT_EQ(raw::signalJobRanges({signal(), signal(), signal()}, 0), (std::vector<size_t>{0, 1, 2}));
}

TEST(EventJob, ItStopsAtAnEncoderAWaitOrAnEventTheKernelDoesNotHold)
{
    EXPECT_EQ(raw::signalJobRanges({signal(), encoder(), signal()}, 0), (std::vector<size_t>{0}));
    EXPECT_EQ(raw::signalJobRanges({signal(), wait(), signal()}, 0), (std::vector<size_t>{0}));
    EXPECT_TRUE(raw::signalJobRanges({signal(false), signal()}, 0).empty());
}

TEST(EventJob, SignalsAJobAlreadyCarriesAreNotSignalledTwice)
{
    EXPECT_EQ(raw::signalJobRanges({signal(), signal(true, true), signal()}, 0), (std::vector<size_t>{0, 2}));
}

}  // namespace
