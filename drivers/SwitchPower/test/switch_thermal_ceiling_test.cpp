// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchThermalCeiling.h against a model DFLL: each path of the ceiling check as its steps, every settle a timer shot
// the work loop returns from, the governor's signals free to run between them. And SwitchThermal.cpp as it stands: no
// sleep left on the paths a governor signal or an ALERT waits behind.
#include "drivers/SwitchPower/SwitchThermalCeiling.h"

#include <gtest/gtest.h>

#include <deque>
#include <string>
#include <vector>

#include "bazel/testing/source_text.h"

namespace {
constexpr uint32_t kStepKhz = 25500;  // a DFLL step
constexpr uint32_t kSettleMs = 10;    // SwitchThermal.cpp kDfllSettleMS
constexpr uint32_t kRows[] = {1581000, 1683000, 1785000};

struct Dfll {
    std::deque<uint32_t> probes;  // what each probe measures next
    unsigned ceilingIndex = 2;
    uint32_t appliedNow = 1785000;
    bool onTrial = false;
    int refuse = 0;
    std::vector<std::string> trace;

    uint32_t probeKhz()
    {
        const uint32_t khz = probes.empty() ? 1785000 : probes.front();
        if (!probes.empty())
            probes.pop_front();
        trace.push_back("probe " + std::to_string(khz / 1000));
        return khz;
    }

    unsigned ceiling() const
    {
        return ceilingIndex;
    }

    static uint32_t khzAt(unsigned index)
    {
        return kRows[index];
    }

    uint32_t appliedKhz() const
    {
        return appliedNow;
    }

    bool landed(unsigned)
    {
        const bool was = onTrial;
        onTrial = false;
        return was;
    }

    unsigned hold(unsigned index, bool *again)
    {
        *again = onTrial;
        trace.push_back("hold " + std::to_string(kRows[index] / 1000));
        ceilingIndex = index - 1;
        return 1;
    }

    int setRate(uint32_t khz)
    {
        trace.push_back("set " + std::to_string(khz / 1000));
        return refuse;
    }

    void arm(uint32_t ms)
    {
        trace.push_back("arm " + std::to_string(ms));
    }

    void logRestored(const switchceiling::Report &)
    {
        trace.push_back("log restored");
    }

    void logReasked(const switchceiling::Report &report)
    {
        trace.push_back(std::string("log reasked") + (report.restored ? " restored" : ""));
    }

    // SwitchThermal's line carries the clock measured once more.
    void logHeld(const switchceiling::Report &report)
    {
        probeKhz();
        trace.push_back(std::string("log held") + (report.retried ? " again" : ""));
    }

    void applied(unsigned index)
    {
        appliedNow = kRows[index];
        trace.push_back("applied " + std::to_string(kRows[index] / 1000));
    }
};

using Trace = std::vector<std::string>;
using Check = switchceiling::Check<Dfll>;
}  // namespace

TEST(SwitchThermalCeiling, RightAtTheCeilingIsOneProbe)
{
    Dfll dfll;
    Check check(kStepKhz, kSettleMs);
    bool moved = true;
    check.start(dfll, &moved);
    EXPECT_FALSE(moved);
    EXPECT_FALSE(check.busy());
    EXPECT_EQ(dfll.trace, (Trace{"probe 1785"}));
}

// One step high, right once asked again from below: each settle is a timer shot the loop returns from.
TEST(SwitchThermalCeiling, HighThenRightAfterTheReaskIsThreeShots)
{
    Dfll dfll;
    dfll.probes = {1810500, 1785000};
    Check check(kStepKhz, kSettleMs);
    bool moved = false;
    check.start(dfll, &moved);
    EXPECT_TRUE(moved);
    EXPECT_EQ(dfll.trace, (Trace{"probe 1810", "set 1683", "arm 2"}));
    ASSERT_TRUE(check.busy());
    dfll.trace.clear();
    EXPECT_FALSE(check.fired(dfll));
    EXPECT_EQ(dfll.trace, (Trace{"set 1785", "arm 10"}));
    dfll.trace.clear();
    EXPECT_TRUE(check.fired(dfll));
    EXPECT_EQ(dfll.trace, (Trace{"probe 1785", "applied 1785", "log reasked"}));
}

TEST(SwitchThermalCeiling, StillHighIsHeldDownAndTheRowBelowApplied)
{
    Dfll dfll;
    dfll.probes = {1810500, 1810500};
    Check check(kStepKhz, kSettleMs);
    bool moved = false;
    check.start(dfll, &moved);
    check.fired(dfll);
    dfll.trace.clear();
    EXPECT_FALSE(check.fired(dfll));
    EXPECT_EQ(dfll.trace, (Trace{"probe 1810", "hold 1785", "set 1683", "arm 10"}));
    dfll.trace.clear();
    EXPECT_TRUE(check.fired(dfll));
    EXPECT_EQ(dfll.trace, (Trace{"applied 1683", "probe 1785", "log held"}));
}

// A governor move to the ceiling is measured after its settle, by the timer, not by a sleep in the move.
TEST(SwitchThermalCeiling, AMoveToTheCeilingIsMeasuredByTheTimer)
{
    Dfll dfll;
    Check check(kStepKhz, kSettleMs);
    check.afterMove(dfll, 10);
    EXPECT_TRUE(check.busy());
    EXPECT_EQ(dfll.trace, (Trace{"arm 10"}));
    dfll.trace.clear();
    EXPECT_TRUE(check.fired(dfll));
    EXPECT_EQ(dfll.trace, (Trace{"probe 1785"}));
}

// A request the DFLL refuses goes straight to the decision: nothing to settle.
TEST(SwitchThermalCeiling, ARefusedReaskHoldsWithoutASettle)
{
    Dfll dfll;
    dfll.probes = {1810500, 1810500};
    dfll.refuse = -22;
    Check check(kStepKhz, kSettleMs);
    bool moved = false;
    check.start(dfll, &moved);
    EXPECT_FALSE(check.busy());
    EXPECT_EQ(dfll.trace,
              (Trace{"probe 1810", "set 1683", "probe 1810", "hold 1785", "set 1683", "probe 1785", "log held"}));
}

TEST(SwitchThermalCeiling, CancelledStepsDoNothing)
{
    Dfll dfll;
    dfll.probes = {1810500};
    Check check(kStepKhz, kSettleMs);
    bool moved = false;
    check.start(dfll, &moved);
    check.cancel();
    dfll.trace.clear();
    EXPECT_TRUE(check.fired(dfll));
    EXPECT_EQ(dfll.trace, Trace{});
}

// SwitchThermal.cpp: no sleep on the paths governor signals and ALERT run on (the governor's action, the ceiling check,
// the ALERT action). The sleeps left are inside one hardware sequence on a path no signal waits behind.
TEST(SwitchThermalCeilingSource, NoSleepWhereTheGovernorOrAlertWait)
{
    const std::string source = switch_ios::test::SourceText("../SwitchThermal.cpp");
    for (const char *signature : {"void SwitchThermal::governorFired(", "void SwitchThermal::applyGovernor(",
                                  "void SwitchThermal::ceilingFired(", "void SwitchThermal::alertFired(",
                                  "void SwitchThermal::alertReleased("}) {
        const std::string body = switch_ios::test::Body(source, signature);
        EXPECT_FALSE(body.empty()) << signature;
        EXPECT_EQ(body.find("IOSleep"), std::string::npos) << signature;
    }
    EXPECT_EQ(source.find("bool SwitchThermal::dfllOverCeiling("), std::string::npos);
}
