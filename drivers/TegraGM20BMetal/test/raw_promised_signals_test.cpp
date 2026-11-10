// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <cstdint>
#include <utility>
#include <vector>
#include "../raw/runtime/raw_event_timeline.h"
#include "../raw/runtime/raw_promised_signals.h"
#include "../raw/runtime/raw_scheduled_wait.h"

namespace {

using raw::RangeKind;

// RawMetalEvents.mm, RawMetalFinish.mm and RawMetalSchedule.mm over one producer command buffer: its raw ranges submitted
// in order, the signals after the last one promised at its submission and published at its completion
struct Producer {
    Producer(std::vector<RangeKind> ranges, std::vector<uint64_t> values, raw::EventTimeline &events,
             uint64_t &signaledValue)
        : ranges(std::move(ranges)), values(std::move(values)), events(events), signaledValue(signaledValue)
    {}

    std::vector<RangeKind> ranges;
    std::vector<uint64_t> values;  // each signal range's
    raw::EventTimeline &events;
    uint64_t &signaledValue;
    std::vector<raw::EventTimeline::Promise> promises;
    size_t next = 0;
    bool headRetired = false;

    static constexpr raw::EventTimeline::Key kEvent = 1;

    // Submits ranges until one blocks: a raw range always goes; a promised signal goes at once; an unpromised one waits for
    // the work before it (the head stays)
    void submit(bool oneChannel = true, bool failed = false)
    {
        while (next < ranges.size()) {
            if (ranges[next] == RangeKind::native) {
                const size_t end = raw::promisedSignalsEnd(
                    [this](size_t at) {
                        return ranges[at];
                    },
                    ranges.size(), next, oneChannel, failed);
                std::vector<raw::EventTimeline::Key> wake;
                for (size_t at = next + 1; at < end; ++at)
                    promises.push_back(events.publish(kEvent, values[at], {uint32_t(next + 1), 1}, wake));
                next = end;
                continue;
            }
            if (ranges[next] == RangeKind::signal)
                return;
            ++next;
        }
        headRetired = true;
    }

    void complete(bool success)
    {
        for (const auto &promise : promises) {
            if (success && signaledValue < promise->value)
                signaledValue = promise->value;
            events.retire(kEvent, promise, success);
        }
        promises.clear();
    }

    raw::HeadWait consumerWait(uint64_t value) const
    {
        if (signaledValue >= value)
            return raw::HeadWait::signal;
        return events.promised(kEvent, value) ? raw::HeadWait::promised : raw::HeadWait::event;
    }
};

TEST(PromisedSignals, TheHeadRetiresWithItsLastRawRange)
{
    raw::EventTimeline events;
    uint64_t value = 0;
    Producer p{{RangeKind::wait, RangeKind::native, RangeKind::native, RangeKind::signal}, {0, 0, 0, 2}, events, value};
    p.submit();
    EXPECT_TRUE(p.headRetired);
    EXPECT_EQ(p.promises.size(), 1u);
}

TEST(PromisedSignals, AValueIsVisibleOnlyOnceTheWorkBeforeItCompleted)
{
    raw::EventTimeline events;
    uint64_t value = 1;
    Producer p{{RangeKind::native, RangeKind::signal, RangeKind::signal}, {0, 2, 3}, events, value};
    p.submit();
    ASSERT_TRUE(p.headRetired);
    EXPECT_EQ(value, 1u);
    EXPECT_EQ(p.consumerWait(2), raw::HeadWait::promised);
    EXPECT_FALSE(raw::holds(p.consumerWait(3)));
    EXPECT_EQ(p.consumerWait(4), raw::HeadWait::event);
    p.complete(true);
    EXPECT_EQ(value, 3u);
    EXPECT_FALSE(events.promised(Producer::kEvent, 2));
    EXPECT_EQ(events.size(), 0u);
}

TEST(PromisedSignals, AFailedProducerPublishesNothingAndReleasesItsConsumersWaiter)
{
    raw::EventTimeline events;
    uint64_t value = 1;
    Producer p{{RangeKind::native, RangeKind::signal}, {0, 2}, events, value};
    p.submit();
    ASSERT_TRUE(p.headRetired);
    const raw::HeadWait before = p.consumerWait(2);
    EXPECT_FALSE(raw::scheduledWaitReturns(false, raw::holds(before), false));
    p.complete(false);
    EXPECT_EQ(value, 1u);
    const raw::HeadWait after = p.consumerWait(2);
    EXPECT_EQ(after, raw::HeadWait::event);
    EXPECT_TRUE(raw::scheduledWaitReturns(false, raw::holds(after), false));
    EXPECT_EQ(events.size(), 0u);
}

TEST(PromisedSignals, AnM2vRangeEarlierInTheBufferKeepsTheBlockingPath)
{
    raw::EventTimeline events;
    uint64_t value = 0;
    Producer p{{RangeKind::compatibility, RangeKind::native, RangeKind::signal}, {0, 0, 2}, events, value};
    p.submit();
    EXPECT_FALSE(p.headRetired);
    EXPECT_EQ(p.next, 2u);
    EXPECT_TRUE(p.promises.empty());
}

TEST(PromisedSignals, AChannelMadeAgainBetweenItsRawRangesKeepsTheBlockingPath)
{
    raw::EventTimeline events;
    uint64_t value = 0;
    Producer p{{RangeKind::native, RangeKind::signal}, {0, 2}, events, value};
    p.submit(false);
    EXPECT_FALSE(p.headRetired);
    EXPECT_TRUE(p.promises.empty());
    Producer failed{{RangeKind::native, RangeKind::signal}, {0, 2}, events, value};
    failed.submit(true, true);
    EXPECT_FALSE(failed.headRetired);
    EXPECT_TRUE(failed.promises.empty());
}

TEST(PromisedSignals, OnlyTheSignalsRightAfterARawRangeArePromised)
{
    const std::vector<RangeKind> ranges{RangeKind::native, RangeKind::signal, RangeKind::wait, RangeKind::signal};
    auto kindAt = [&](size_t at) {
        return ranges[at];
    };
    EXPECT_EQ(raw::promisedSignalsEnd(kindAt, ranges.size(), 0, true, false), 2u);
    const std::vector<RangeKind> none{RangeKind::native, RangeKind::native};
    EXPECT_EQ(raw::promisedSignalsEnd(
                  [&](size_t at) {
                      return none[at];
                  },
                  none.size(), 0, true, false),
              1u);
}

}  // namespace
