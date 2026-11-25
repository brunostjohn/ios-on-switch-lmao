// kernel/switch/sched/callouts.h on the host: thread call functions and timer actions by CPU, per CPU table and
// summed, each window's top four and the rest; both trees bracket thread_call_invoke's function and
// IOTimerEventSource's thread call actions.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <memory>
#include <string>

#include "bazel/testing/source_text.h"
#include "sched/callouts.h"

using testing::HasSubstr;

namespace {

constexpr uint64_t kMs = 1000000;

uint64_t Same(uint64_t address)
{
    return address;
}

struct Report {
    std::unique_ptr<switch_callout_sum> seen = std::make_unique<switch_callout_sum>();

    std::string Window(const switch_callout_table *tables, int count)
    {
        auto now = std::make_unique<switch_callout_sum>();
        for (int i = 0; i < count; i++)
            switch_callout_sum_table(now.get(), &tables[i]);
        char line[400];
        switch_callout_format(line, sizeof(line), now.get(), seen.get(), Same, 1, 1);
        return line;
    }
};

TEST(XnuSchedCallouts, EachWindowNamesItsBusiestFunctionsAcrossCpus)
{
    auto cpus = std::make_unique<switch_callout_table[]>(2);
    switch_callout_add(&cpus[0], 0xa000, 700 * kMs);
    switch_callout_add(&cpus[1], 0xa000, 36 * kMs);
    switch_callout_add(&cpus[1], 0xb000, 12 * kMs);
    for (uint64_t f = 1; f <= 4; f++)
        switch_callout_add(&cpus[0], 0xc000 + f, f * kMs);
    Report r;
    EXPECT_EQ(r.Window(cpus.get(), 2),
              "0xa000 736 ms (2), 0xb000 12 ms (1), 0xc004 4 ms (1), 0xc003 3 ms (1); others 3 ms (2)");
    switch_callout_add(&cpus[1], 0xb000, 5 * kMs);
    EXPECT_EQ(r.Window(cpus.get(), 2), "0xb000 5 ms (1); others 0 ms (0)") << "the window's share only";
}

TEST(XnuSchedCallouts, AFullTableCountsTheRestInItsOtherRow)
{
    auto t = std::make_unique<switch_callout_table>();
    for (uint64_t f = 1; f <= SWITCH_CALLOUT_ROWS + 2; f++)
        switch_callout_add(t.get(), f << 12, kMs);
    EXPECT_EQ(t->other.calls, 2u);
    Report r;
    EXPECT_THAT(r.Window(t.get(), 1), HasSubstr("; others 62 ms (62)"));
}

class XnuSchedCalloutsSource : public testing::TestWithParam<const char *> {};

TEST_P(XnuSchedCalloutsSource, ThreadCallFunctionsAndTimerActionsAreBracketed)
{
    const std::string tree = GetParam();
    const std::string calls = switch_ios::test::SourceText("../" + tree + "/osfmk/kern/thread_call.c");
    EXPECT_THAT(switch_ios::test::Body(calls, "\nthread_call_invoke(thread_call_func_t func"),
                HasSubstr("\tconst uint64_t switch_begin = switch_callout_begin();\n\t(*func)(param0, param1);\n"
                          "\tswitch_callout_end(SWITCH_CALLOUT_FUNCTION, (const void *)func, switch_begin);\n"));
    const std::string timer = switch_ios::test::SourceText("../" + tree + "/iokit/Kernel/IOTimerEventSource.cpp");
    const std::string bracketed = "const uint64_t switch_begin = switch_callout_begin();\n\t\t\t\tme->invokeAction(doit, "
                                  "me, me->owner, me->workLoop);\n\t\t\t\tswitch_callout_end(SWITCH_CALLOUT_TIMER_"
                                  "ACTION, (const void *)doit, switch_begin);\n";
    EXPECT_THAT(switch_ios::test::Body(timer, "\nIOTimerEventSource::timeout(void *self)"), HasSubstr(bracketed));
    EXPECT_THAT(switch_ios::test::Body(timer, "\nIOTimerEventSource::timeoutAndRelease(void * self"),
                HasSubstr(bracketed));
}

INSTANTIATE_TEST_SUITE_P(BothTrees, XnuSchedCalloutsSource, testing::Values("xnu-10063.141.1", "xnu-12377.121.6"));

}  // namespace
