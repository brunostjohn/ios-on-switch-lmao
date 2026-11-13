// kernel/switch/idle/idle.c's C7 requests on the host: a refusal from the firmware is tried again at later idles of
// that core alone, backing off, and the idle path never locks, sleeps, allocates or logs while it runs masked.
#include "kits/SwitchIdle/switch_idle_governor.h"
#include "kits/SwitchIdle/test/switch_idle_kernel_stubs.h"

#include <switch/idle.h>

#include "bazel/testing/fresh_process.h"

#include <gtest/gtest.h>

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

extern "C" {
int idle_test_masked;
unsigned idle_test_violations;
const char *idle_test_violation;
uint64_t idle_test_now = 1'000 * SWITCH_IDLE_TICKS_US(1000);
uint64_t idle_test_wfi_until;
unsigned idle_test_thread_call_enters;
}

namespace {
constexpr uint64_t kMs = SWITCH_IDLE_TICKS_US(1000);
constexpr int64_t kDenied = -3;  // PSCI_E_DENIED
constexpr int64_t kTaken = 0;    // CPU_SUSPEND returned at once for a pending interrupt

cpu_data gCurrent;
thread_call_func_t gReport;
std::vector<std::string> gLines;
int64_t gAnswer[SWITCH_SMP_CORE_COUNT];
unsigned gRequests[SWITCH_SMP_CORE_COUNT];

void mayNotRunMasked(const char *what)
{
    if (idle_test_masked) {
        ++idle_test_violations;
        idle_test_violation = what;
    }
}

void record(const char *format, va_list args)
{
    char line[256];
    std::vsnprintf(line, sizeof(line), format, args);
    gLines.emplace_back(line);
}
}  // namespace

extern "C" {
uint64_t mach_absolute_time(void)
{
    return idle_test_now;
}

cpu_data_t *getCpuDatap(void)
{
    return &gCurrent;
}

void ClearIdlePop(int)
{}

void switch_secondary_vectors_leave(void)
{}

void idle_test_wfi(void)
{
    if (idle_test_wfi_until > idle_test_now)
        idle_test_now = idle_test_wfi_until;
}

thread_call_t thread_call_allocate_with_options(thread_call_func_t func, thread_call_param_t, int, unsigned)
{
    mayNotRunMasked("thread_call_allocate_with_options");
    gReport = func;
    return reinterpret_cast<thread_call_t>(&gReport);
}

bool thread_call_enter(thread_call_t)
{
    ++idle_test_thread_call_enters;
    return true;
}

void os_log(void *, const char *format, ...)
{
    mayNotRunMasked("os_log");
    va_list args;
    va_start(args, format);
    record(format, args);
    va_end(args);
}

void os_log_error(void *, const char *format, ...)
{
    mayNotRunMasked("os_log_error");
    va_list args;
    va_start(args, format);
    record(format, args);
    va_end(args);
}

void lck_mtx_lock(void *)
{
    mayNotRunMasked("lck_mtx_lock");
}

void *kalloc_data(size_t, int)
{
    mayNotRunMasked("kalloc_data");
    return nullptr;
}

void *IOMalloc(size_t)
{
    mayNotRunMasked("IOMalloc");
    return nullptr;
}

void IOSleep(unsigned)
{
    mayNotRunMasked("IOSleep");
}

void delay(int)
{
    mayNotRunMasked("delay");
}

int thread_block(void *)
{
    mayNotRunMasked("thread_block");
    return 0;
}
}

namespace {
int64_t powerDown(uint32_t cpu, uint64_t)
{
    ++gRequests[cpu];
    idle_test_now += 1;
    return gAnswer[cpu];
}

void resumed(uint32_t)
{}

const switch_idle_platform kPlatform = {powerDown, resumed, uint64_t{1} << 29};

// One idle of `cpu` whose timer is 100 ms away, masked as machine_idle runs it: WFI lasts to the timer, a C7 request
// returns at once with the core's answer.
void idle(uint32_t cpu)
{
    gCurrent = {static_cast<int>(cpu), idle_test_now + 100 * kMs};
    idle_test_wfi_until = gCurrent.rtcPop;
    idle_test_masked = 1;
    switch_idle_wfi(&gCurrent);
    idle_test_masked = 0;
}

// The core back from a power-down through XNU's reset path (arm_init_idle_cpu), masked.
void resume(uint32_t cpu)
{
    gCurrent = {static_cast<int>(cpu), idle_test_now + 100 * kMs};
    idle_test_masked = 1;
    switch_idle_resumed(&gCurrent);
    idle_test_masked = 0;
}

// Idles of `cpu` until it has requested C7 `requests` more times (at most `limit` idles); returns the idles it took.
unsigned idleUntilRequests(uint32_t cpu, unsigned requests, unsigned limit = 100'000)
{
    const unsigned want = gRequests[cpu] + requests;
    unsigned idles = 0;
    while (gRequests[cpu] < want && idles < limit) {
        idle(cpu);
        ++idles;
    }
    return idles;
}

void runReport()
{
    ASSERT_NE(gReport, nullptr);
    gReport(nullptr, nullptr);
}

void start()
{
    switch_idle_start(&kPlatform);
}
}  // namespace

TEST(SwitchIdleRetry, ARefusedRequestIsMadeAgainAtALaterIdle)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        start();
        gAnswer[0] = kDenied;
        ASSERT_LT(idleUntilRequests(0, 1), 10u);
        gAnswer[0] = kTaken;
        EXPECT_LT(idleUntilRequests(0, 1, 50), 50u);
        EXPECT_EQ(idle_test_violations, 0u) << idle_test_violation;
    });
}

TEST(SwitchIdleRetry, OneCoresRefusalsLeaveTheOtherCoresC7)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        start();
        gAnswer[0] = kDenied;
        gAnswer[1] = kTaken;
        for (unsigned i = 0; i != 200; ++i) {
            idle(0);
            idle(1);
        }
        EXPECT_GE(gRequests[0], SWITCH_IDLE_C7_TRIES);
        EXPECT_GE(gRequests[1], 50u);
        EXPECT_EQ(idle_test_violations, 0u) << idle_test_violation;
    });
}

TEST(SwitchIdleRetry, ACoreTheFirmwareAlwaysRefusesBacksOffAndKeepsTrying)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        start();
        gAnswer[2] = kDenied;
        for (unsigned i = 0; i != 40'000; ++i)
            idle(2);
        const unsigned early = gRequests[2];
        EXPECT_GT(early, SWITCH_IDLE_C7_TRIES + SWITCH_IDLE_C7_WAIT_LOG);
        EXPECT_LT(early, 64u);
        for (unsigned i = 0; i != 3 * SWITCH_IDLE_C7_WAIT_MAX; ++i)
            idle(2);
        EXPECT_GE(gRequests[2], early + 1);
        EXPECT_LE(gRequests[2], early + 3);
        EXPECT_EQ(idle_test_violations, 0u) << idle_test_violation;
    });
}

TEST(SwitchIdleRetry, ARequestTheFirmwareTakesStartsTheCoreOver)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        start();
        gAnswer[0] = kDenied;
        idleUntilRequests(0, SWITCH_IDLE_C7_TRIES + 3);
        gAnswer[0] = kTaken;
        EXPECT_GT(idleUntilRequests(0, 1), 8u);
        EXPECT_LE(idleUntilRequests(0, 1), 2u);
        EXPECT_LE(idleUntilRequests(0, 10), 20u);
        gAnswer[0] = kDenied;
        idleUntilRequests(0, SWITCH_IDLE_C7_TRIES + 3);
        resume(0);
        EXPECT_LE(idleUntilRequests(0, 1), 2u);
        EXPECT_EQ(idle_test_violations, 0u) << idle_test_violation;
    });
}

TEST(SwitchIdleRetry, TheThreadCallLogsEachDueLineAndOnlyThen)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        start();
        gAnswer[3] = kDenied;
        idleUntilRequests(3, 1);
        EXPECT_EQ(idle_test_thread_call_enters, 1u);
        runReport();
        idleUntilRequests(3, 2);
        EXPECT_EQ(idle_test_thread_call_enters, 2u);
        idleUntilRequests(3, 2);
        EXPECT_EQ(idle_test_thread_call_enters, 3u);
        runReport();
        idleUntilRequests(3, 11);
        EXPECT_EQ(idle_test_thread_call_enters, 5u);
        gAnswer[3] = kTaken;
        idleUntilRequests(3, 1);
        EXPECT_EQ(idle_test_thread_call_enters, 6u);
        idleUntilRequests(3, 5);
        EXPECT_EQ(idle_test_thread_call_enters, 6u);
        runReport();
        const std::vector<std::string> want = {
            "SwitchIdle: cpu3: PSCI CPU_SUSPEND refused C7, 1 in a row (last returned -3); C7 is requested again at "
            "later idles",
            "SwitchIdle: cpu3: PSCI CPU_SUSPEND refused C7, 4 in a row (last returned -3); C7 is requested again at "
            "later idles",
            "SwitchIdle: cpu3: PSCI CPU_SUSPEND refused C7, 16 in a row (last returned -3); C7 is requested again at "
            "later idles",
            "SwitchIdle: cpu3: C7 taken again; refusals in a row before it: 16",
        };
        EXPECT_EQ(gLines, want);
        EXPECT_EQ(idle_test_violations, 0u) << idle_test_violation;
    });
}

TEST(SwitchIdleRetry, WhatMayNotRunMaskedIsCaughtOnlyMasked)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        lck_mtx_lock(nullptr);
        (void)kalloc_data(16, 0);
        os_log_error(OS_LOG_DEFAULT, "unmasked");
        EXPECT_EQ(idle_test_violations, 0u);
        idle_test_masked = 1;
        lck_mtx_lock(nullptr);
        (void)kalloc_data(16, 0);
        (void)IOMalloc(16);
        IOSleep(1);
        delay(1);
        (void)thread_block(nullptr);
        os_log(OS_LOG_DEFAULT, "masked");
        os_log_error(OS_LOG_DEFAULT, "masked");
        idle_test_masked = 0;
        EXPECT_EQ(idle_test_violations, 8u);
    });
}
