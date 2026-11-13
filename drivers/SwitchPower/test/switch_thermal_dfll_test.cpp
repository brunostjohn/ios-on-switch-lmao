// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchThermalDfll.h, SwitchThermal's DFLL mode, run against a stand-in SwitchThermal and a DFLL whose answers the
// test sets: a failure leaves the CPU on the PLL ladder and the DFLL is tried again once its wait is over, the waits
// doubling to 512 s; only a chip the DFLL does not fit (-ENODEV from prepare) ends it for the boot; a refused rate
// keeps the DFLL and its governor. Also SwitchGovernor.h's retry arithmetic and the governor's askable row.
#include <gtest/gtest.h>

#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../../TegraDFLL/TegraDFLL.h"
#include "../SwitchGovernor.h"
#include "../SwitchPerfGovernor.h"
#include "bazel/testing/source_text.h"

namespace {
constexpr uint64_t kSecond = 1000;  // mach ticks in this test
constexpr int kLinuxEAGAIN = -11, kLinuxEIO = -5;
constexpr unsigned long kDfllSafeHz = 1683000000UL;

uint64_t gNow = 5 * kSecond;
std::vector<std::string> gLines;
int gPrepare, gEnable, gResume;
unsigned gPrepares, gEnables, gResumes, gRefusedCalls;
unsigned gRefusedInRow[SWITCH_GOV_MAX_OPPS];

bool has(const char *text)
{
    for (const auto &line : gLines)
        if (line.find(text) != std::string::npos)
            return true;
    return false;
}
}  // namespace

uint64_t mach_absolute_time()
{
    return gNow;
}

__attribute__((format(printf, 1, 2))) void IOLog(const char *format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    gLines.emplace_back(line);
}

extern "C" {
int tegra_dfll_cpu_prepare(char *why, size_t len)
{
    ++gPrepares;
    snprintf(why, len, "prepare %d", gPrepare);
    return gPrepare;
}

int tegra_dfll_cpu_enable(unsigned long, char *why, size_t len)
{
    ++gEnables;
    snprintf(why, len, "enable %d", gEnable);
    return gEnable;
}

int tegra_dfll_cpu_resume(char *why, size_t len)
{
    ++gResumes;
    snprintf(why, len, "resume %d", gResume);
    return gResume;
}

void tegra_dfll_cpu_describe(unsigned long, char *buf, size_t len)
{
    snprintf(buf, len, "closed loop");
}
}

unsigned switchperf::refused(unsigned index)
{
    ++gRefusedCalls;
    return ++gRefusedInRow[index];
}

namespace switchclk {
struct Cpu {
    unsigned kHz;
};

Cpu readCpu(volatile uint8_t *)
{
    return {1785000};
}
}  // namespace switchclk

class SwitchThermal {
public:
    volatile uint8_t *car = nullptr;
    bool dfllWanted = true, dfllMode = true, dfllPrepared = false, governed = true, dfllResume = false,
         dfllQuiet = false;
    switch_gov_retry dfllRetry = {};
    uint64_t dfllRetryUnit = kSecond;
    bool perfRaised = false, perfSettled = false;
    unsigned perfRaises = 0, dfllFloor = 0, dfllCap = 0;
    unsigned long dfllTopHz = 0;
    bool governorStarts = true, ladderLowers = true;
    unsigned governorStartsAsked = 0, restores = 0, lowered = 0, ladderRaises = 0, ladderLines = 0, thermalUpdates = 0;

    void dfllFailed(const char *what, const char *why);
    bool dfllAgain();
    bool retryDfll(int soc);
    void resumeDfll();
    void dfllRefused(unsigned index, unsigned long hz, int err);
    bool raiseDfll(int soc);

    // raiseCpu's DFLL half as SwitchThermal.cpp runs it, then the PLL ladder as one step and its line.
    bool raiseCpu(int soc)
    {
        dfllQuiet = false;
        (void)dfllAgain();
        if (dfllMode && raiseDfll(soc))
            return true;
        perfRaised = true;
        ++ladderRaises;
        if (!dfllQuiet)
            ++ladderLines;
        return true;
    }

    void restoreCpu(const char *, bool, bool keepDfll = false)
    {
        if (!perfRaised)
            return;
        perfRaised = false;
        ++restores;
        if (dfllMode)
            dfllMode = keepDfll;
    }

    bool lowerLadder(const char *, int *)
    {
        ++lowered;
        return ladderLowers;
    }

    bool startGovernor(char *why, size_t len)
    {
        ++governorStartsAsked;
        snprintf(why, len, "%s", governorStarts ? "CPU governor: 9 points" : "no work loop source");
        return governorStarts;
    }

    void updateDfllThermal(int)
    {
        ++thermalUpdates;
    }

    static unsigned long settleDfllTop()
    {
        return 1785000000UL;
    }

    void publish(const char *, int64_t)
    {}

    bool onDfll() const
    {
        return perfRaised && dfllMode;
    }

    bool onLadder() const
    {
        return perfRaised && !dfllMode;
    }
};

#include "../SwitchThermalDfll.h"

namespace {
class ThermalDfll : public ::testing::Test {
protected:
    SwitchThermal thermal;

    void SetUp() override
    {
        gNow = 5 * kSecond;
        gLines.clear();
        gPrepare = gEnable = gResume = 0;
        gPrepares = gEnables = gResumes = gRefusedCalls = 0;
        for (unsigned &row : gRefusedInRow)
            row = 0;
    }

    // An evaluation (a SOC_THERM degree, the ALERT) `seconds` later, below the CPU's thermal trip.
    bool evaluate(uint64_t seconds)
    {
        gNow += seconds * kSecond;
        return thermal.retryDfll(40000);
    }
};

TEST_F(ThermalDfll, AFailedPrepareIsTriedAgainOnceItsWaitIsOver)
{
    gPrepare = kLinuxEAGAIN;
    ASSERT_TRUE(thermal.raiseCpu(40000));
    EXPECT_TRUE(thermal.onLadder());
    EXPECT_TRUE(has("SwitchPerf: DFLL not used (-11: prepare -11); the PLL ladder until the DFLL is tried again, 1 "
                    "failed in a row"));
    gPrepare = 0;
    gNow += kSecond - 1;
    EXPECT_FALSE(thermal.retryDfll(40000));
    EXPECT_EQ(gPrepares, 1u);
    gNow += 1;
    EXPECT_TRUE(thermal.retryDfll(40000));
    EXPECT_TRUE(thermal.onDfll());
    EXPECT_TRUE(thermal.governed);
    EXPECT_EQ(thermal.lowered, 1u);
    EXPECT_EQ(gPrepares, 2u);
    EXPECT_EQ(thermal.dfllRetry.failures, 0u);
    EXPECT_TRUE(has("; the DFLL works again after 1 failed tries: closed loop"));
}

TEST_F(ThermalDfll, AFailedEnableAndAGovernorThatDidNotStartAreTriedAgain)
{
    gEnable = kLinuxEIO;
    ASSERT_TRUE(thermal.raiseCpu(40000));
    EXPECT_TRUE(thermal.onLadder());
    gEnable = 0;
    thermal.governorStarts = false;
    EXPECT_TRUE(evaluate(1));
    EXPECT_TRUE(thermal.onLadder());
    EXPECT_EQ(thermal.restores, 1u);
    EXPECT_TRUE(has("SwitchPerf: CPU governor not started (no work loop source); the PLL ladder until the DFLL is "
                    "tried again, 2 failed in a row"));
    thermal.governorStarts = true;
    EXPECT_FALSE(evaluate(1));
    EXPECT_TRUE(evaluate(1));
    EXPECT_TRUE(thermal.onDfll());
    EXPECT_TRUE(thermal.governed);
    EXPECT_EQ(gEnables, 3u);
    EXPECT_EQ(gPrepares, 1u);
}

TEST_F(ThermalDfll, WaitsDoubleToTheirCapAndOnlyPowersOfTwoAreLogged)
{
    gPrepare = kLinuxEAGAIN;
    ASSERT_TRUE(thermal.raiseCpu(40000));
    std::vector<uint64_t> waits;
    for (unsigned failures = 1; failures != 14; ++failures) {
        const uint64_t due = thermal.dfllRetry.due;
        waits.push_back((due - gNow) / kSecond);
        gNow = due - 1;
        EXPECT_FALSE(thermal.retryDfll(40000));
        gNow = due;
        EXPECT_TRUE(thermal.retryDfll(40000));
        EXPECT_TRUE(thermal.onLadder());
    }
    const std::vector<uint64_t> want = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 512, 512, 512};
    EXPECT_EQ(waits, want);
    EXPECT_EQ(gPrepares, 14u);
    EXPECT_EQ(gLines.size(), 4u);  // failures 1, 2, 4, 8
    EXPECT_EQ(thermal.ladderRaises, 14u);
    EXPECT_EQ(thermal.ladderLines, 4u);
    EXPECT_TRUE(has("8 failed in a row"));
}

TEST_F(ThermalDfll, AChipTheDfllDoesNotFitKeepsThePllModeForTheBoot)
{
    gPrepare = -ENODEV;
    ASSERT_TRUE(thermal.raiseCpu(40000));
    EXPECT_TRUE(thermal.onLadder());
    EXPECT_FALSE(thermal.dfllWanted);
    EXPECT_TRUE(has("SwitchPerf: the DFLL does not fit this chip (prepare -19); PLL mode for this boot"));
    gPrepare = 0;
    EXPECT_FALSE(evaluate(10'000));
    EXPECT_EQ(gPrepares, 1u);
    thermal.resumeDfll();
    EXPECT_EQ(gResumes, 0u);
}

TEST_F(ThermalDfll, ARestoreAfterSleepThatFailedIsDoneBeforeTheNextTry)
{
    thermal.dfllPrepared = true;
    gResume = -EBUSY;
    thermal.resumeDfll();
    EXPECT_TRUE(thermal.dfllResume);
    EXPECT_FALSE(thermal.dfllMode);
    EXPECT_TRUE(has("SwitchPerf: DFLL not restored after sleep (-16: resume -16)"));
    ASSERT_TRUE(thermal.raiseCpu(40000));
    EXPECT_TRUE(thermal.onLadder());
    EXPECT_EQ(gEnables, 0u);
    EXPECT_FALSE(evaluate(1));
    EXPECT_EQ(gResumes, 2u);
    gResume = 0;
    EXPECT_TRUE(evaluate(2));
    EXPECT_EQ(gResumes, 3u);
    EXPECT_FALSE(thermal.dfllResume);
    EXPECT_TRUE(thermal.onDfll());
}

TEST_F(ThermalDfll, ALadderThatDoesNotComeDownStaysWhereItIs)
{
    gEnable = kLinuxEIO;
    ASSERT_TRUE(thermal.raiseCpu(40000));
    gEnable = 0;
    thermal.ladderLowers = false;
    EXPECT_FALSE(evaluate(1));
    EXPECT_FALSE(thermal.dfllMode);
    EXPECT_EQ(gEnables, 1u);
}

TEST_F(ThermalDfll, ARefusedRateKeepsTheDfllAndItsGovernor)
{
    ASSERT_TRUE(thermal.raiseCpu(40000));
    ASSERT_TRUE(thermal.onDfll());
    for (unsigned i = 0; i != 5; ++i)
        thermal.dfllRefused(8, 1785000000UL, -22);
    EXPECT_TRUE(thermal.onDfll());
    EXPECT_TRUE(thermal.governed);
    EXPECT_EQ(thermal.restores, 0u);
    EXPECT_EQ(gRefusedCalls, 5u);
    EXPECT_TRUE(
        has("SwitchPerf: the DFLL refused 1785 MHz (-22), 1 in a row; the governor asks it again after a wait"));
    EXPECT_TRUE(has("2 in a row"));
    EXPECT_TRUE(has("4 in a row"));
    EXPECT_FALSE(has("3 in a row"));
    EXPECT_FALSE(has("5 in a row"));
}

// What a stand-in cannot run: SwitchThermal.cpp hands each DFLL event to the fragment.
TEST(ThermalDfllSource, SwitchThermalHandsEveryDfllEventToTheFragment)
{
    const std::string source = switch_ios::test::SourceText("../SwitchThermal.cpp");
    EXPECT_NE(switch_ios::test::Body(source, "bool SwitchThermal::evaluate(").find("retryDfll(soc)"),
              std::string::npos);
    EXPECT_NE(switch_ios::test::Body(source, "bool SwitchThermal::raiseCpu(").find("dfllAgain()"), std::string::npos);
    const std::string fired = switch_ios::test::Body(source, "void SwitchThermal::governorFired(");
    EXPECT_NE(fired.find("dfllRefused(index, hz, err)"), std::string::npos);
    EXPECT_NE(fired.find("dfllFailed("), std::string::npos);
    EXPECT_NE(switch_ios::test::Body(source, "void SwitchThermal::resume(").find("resumeDfll()"), std::string::npos);
    EXPECT_EQ(source.find("PLL mode for this boot"), std::string::npos);
}

TEST(SwitchGovRetry, WaitsDoubleFromTheUnitToTheCap)
{
    switch_gov_retry r = {};
    EXPECT_TRUE(switch_gov_retry_due(&r, 0));
    EXPECT_TRUE(switch_gov_retry_failed(&r, 100, 3));
    EXPECT_EQ(r.due, 103u);
    EXPECT_FALSE(switch_gov_retry_due(&r, 102));
    EXPECT_TRUE(switch_gov_retry_due(&r, 103));
    EXPECT_TRUE(switch_gov_retry_failed(&r, 103, 3));
    EXPECT_EQ(r.due, 109u);
    EXPECT_FALSE(switch_gov_retry_failed(&r, 109, 3));
    EXPECT_EQ(r.due, 121u);
    for (unsigned i = 0; i != 20; ++i)
        (void)switch_gov_retry_failed(&r, 1000, 3);
    EXPECT_EQ(r.due, 1000u + (3u << SWITCH_GOV_RETRY_LOG));
    EXPECT_EQ(switch_gov_retry_cleared(&r), 23u);
    EXPECT_TRUE(switch_gov_retry_due(&r, 0));
}

TEST(SwitchGovRetry, TheGovernorAsksTheNearestRowItMayWhileOneWaits)
{
    switch_gov_retry refused[SWITCH_GOV_MAX_OPPS] = {};
    EXPECT_EQ(switch_gov_askable(refused, 8, 3, 0), 8u);
    (void)switch_gov_retry_failed(&refused[8], 10, 1);
    (void)switch_gov_retry_failed(&refused[7], 10, 4);
    EXPECT_EQ(switch_gov_askable(refused, 8, 3, 10), 6u);
    EXPECT_EQ(switch_gov_askable(refused, 8, 3, 11), 8u);
    EXPECT_EQ(switch_gov_askable(refused, 7, 3, 11), 6u);
    EXPECT_EQ(switch_gov_askable(refused, 7, 9, 11), 8u);
    EXPECT_EQ(switch_gov_askable(refused, 8, 7, 10), 7u);
    EXPECT_EQ(switch_gov_askable(refused, 3, 3, 10), 3u);
}
}  // namespace
