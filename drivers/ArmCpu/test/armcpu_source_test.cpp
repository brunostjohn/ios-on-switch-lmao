// XNU takes ml_processor_info_t's powergate_latency as cpu_idle_latency (both trees' machine_routines.c). SetIdlePop
// then wakes every idle core that long before its timer, and the idle loop WFE-polls the rest of the wait
// (cpu_proximate_timer). WFI only clock-gates and C7 has its own early wake (switch_idle_c7_wake_at), so no idle state
// here pays a power-gate exit on XNU's timer and the latency stays 0.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

TEST(ArmCpuSource, RegistersEachCoreWithNoIdleLatency)
{
    const std::string source = SourceText("../ArmCpu.cpp");
    EXPECT_THAT(source, HasSubstr("ml_processor_register(&info"));
    EXPECT_THAT(source, Not(HasSubstr("powergate_latency")));
}
