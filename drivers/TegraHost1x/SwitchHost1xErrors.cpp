// SPDX-License-Identifier: BSD-2-Clause-Patent
// host1x's general interrupt (SwitchHost1xErrors.h). The filter takes the channels whose illegal opcode is pending
// (masked from then on, status cleared) for the action, on host1x's work loop. The action names each channel in one
// line and fails its jobs (switch_host1x_channels_failed), which only expires fences and settles them, as a bound
// does: no step there sleeps or waits on the loop it runs on. Debug kernels can expect one channel's error (the illegal
// opcode probe): the action then wakes the probe instead.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/clock.h>
#include "SwitchHost1xErrors.h"
#include "SwitchHost1xChannels.h"
#include "TegraHost1x.h"
#include "../ArmGIC/SwitchInterrupts.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
#include "SwitchHost1xErrorProbe.h"
#endif

namespace {
using namespace switch_host1x_errors;

constexpr uint32_t kGeneralIntid = 32 + 67;  // "host" (tegra210.dtsi host1x@50000000)

struct ErrorRegisters {
    volatile uint8_t *sync;

    uint32_t read(uint32_t offset) const
    {
        return switch_mmio_read32(sync, offset);
    }

    void write(uint32_t offset, uint32_t value) const
    {
        switch_mmio_write32(sync, offset, value);
    }
};

ErrorRegisters gErrorRegisters;
Unit<ErrorRegisters> gErrorsUnit;
IOSimpleLock *gErrorsLock;  // gErrorsUnit and HINTMASK_EXT: the filter's take against a restart's rearm
IOFilterInterruptEventSource *gErrorsSource;
uint32_t gErrorsPending;  // the filter's channels, for the action

#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
IOLock *gExpectLock;
uint32_t gExpectedChannel = kChannels;  // kChannels: none
uint32_t gExpectedTaken;

bool takeExpected(uint32_t channel)
{
    IOLockLock(gExpectLock);
    const bool expected = channel == gExpectedChannel;
    if (expected) {
        ++gExpectedTaken;
        IOLockWakeup(gExpectLock, &gExpectedTaken, false);
    }
    IOLockUnlock(gExpectLock);
    return expected;
}
#endif

// NOLINTNEXTLINE(misc-const-correctness): IOFilterInterruptEventSource::Filter's signature
bool errorsFilter(OSObject *, IOFilterInterruptEventSource *)
{
    IOSimpleLockLock(gErrorsLock);
    const uint32_t taken = gErrorsUnit.take(gErrorRegisters);
    IOSimpleLockUnlock(gErrorsLock);
    if (!taken)
        return false;
    __atomic_fetch_or(&gErrorsPending, taken, __ATOMIC_RELEASE);
    return true;
}

void channelStopped(uint32_t channel)
{
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
    if (takeExpected(channel))
        return;
#endif
    char host[200], syncpts[64];
    const uint32_t stat = gErrorRegisters.read(kCmdprocStat);
    tegra_host1x_describe_channel(channel, host, sizeof(host));
    switch_host1x_channels_failed(channel, syncpts, sizeof(syncpts));
    IOLog("TegraHost1x: channel %u stopped on an illegal opcode (CMDPROC_STAT %#x): %s; its jobs fail now (%s%s)\n",
          channel, stat, host, syncpts[0] ? "syncpoints " : "no fenced job in flight", syncpts);
}

void errorsOccurred(OSObject *, IOInterruptEventSource *, int)
{
    for (uint32_t channels = __atomic_exchange_n(&gErrorsPending, 0, __ATOMIC_ACQUIRE); channels;
         channels &= channels - 1)
        channelStopped(static_cast<uint32_t>(__builtin_ctz(channels)));
}

bool errorsConfigure()
{
    uint32_t latched = 0, hintmaskExt = 0;
    IOInterruptState state = IOSimpleLockLockDisableInterrupt(gErrorsLock);
    const bool on = gErrorsUnit.configure(gErrorRegisters, &latched, &hintmaskExt);
    IOSimpleLockUnlockEnableInterrupt(gErrorsLock, state);
    if (latched)
        IOLog("TegraHost1x: illegal opcodes latched before the general interrupt was on: channels %#x (cleared)\n",
              latched);
    if (hintmaskExt != kIllegalAll)
        IOLog("TegraHost1x: general interrupt: HINTMASK_EXT reads %#x after %#x was written\n", hintmaskExt,
              kIllegalAll);
    if (!on)
        IOLog("TegraHost1x: general interrupt (INTID %u) not enabled: INTMASK reads %#x; a stopped channel's jobs fail "
              "only at their bound\n",
              kGeneralIntid, gErrorRegisters.read(kIntMask));
    return on;
}
}  // namespace

bool switch_host1x_errors_start(IOWorkLoop *loop, volatile uint8_t *sync)
{
    gErrorRegisters.sync = sync;
    gErrorsLock = IOSimpleLockAlloc();
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
    gExpectLock = IOLockAlloc();
    if (!gExpectLock)
        gErrorsLock = nullptr;
#endif
    IOService *nub = gErrorsLock ? switch_interrupt_nub(kGeneralIntid, false) : nullptr;
    IOFilterInterruptEventSource *made =
        nub ? IOFilterInterruptEventSource::filterInterruptEventSource(loop, &errorsOccurred, &errorsFilter, nub, 0)
            : nullptr;
    if (!made || loop->addEventSource(made) != kIOReturnSuccess) {
        IOLog("TegraHost1x: general interrupt (INTID %u) could not be attached: a stopped channel's jobs fail only at "
              "their bound\n",
              kGeneralIntid);
        return false;
    }
    if (!errorsConfigure())
        return false;
    made->enable();
    __atomic_store_n(&gErrorsSource, made, __ATOMIC_RELEASE);
    return true;
}

void switch_host1x_errors_stop()
{
    if (IOFilterInterruptEventSource *source = __atomic_load_n(&gErrorsSource, __ATOMIC_ACQUIRE))
        source->disable();
}

void switch_host1x_errors_resume()
{
    IOFilterInterruptEventSource *source = __atomic_load_n(&gErrorsSource, __ATOMIC_ACQUIRE);
    if (source && errorsConfigure())
        source->enable();
}

extern "C" void switch_host1x_errors_rearm(uint32_t channel)
{
    if (!gErrorsLock || !__atomic_load_n(&gErrorsSource, __ATOMIC_ACQUIRE))
        return;
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(gErrorsLock);
    gErrorsUnit.rearm(gErrorRegisters, channel);
    IOSimpleLockUnlockEnableInterrupt(gErrorsLock, state);
}

#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
bool switch_host1x_errors_attached()
{
    return __atomic_load_n(&gErrorsSource, __ATOMIC_ACQUIRE) != nullptr;
}

void switch_host1x_errors_expect(uint32_t channel)
{
    IOLockLock(gExpectLock);
    gExpectedChannel = channel;
    gExpectedTaken = 0;
    IOLockUnlock(gExpectLock);
}

uint32_t switch_host1x_errors_expect_end(uint32_t ms)
{
    const uint32_t channel = gExpectedChannel, bit = 1u << channel;
    uint64_t deadline = 0;
    clock_interval_to_deadline(ms, kMillisecondScale, &deadline);
    IOLockLock(gExpectLock);
    while (!gExpectedTaken &&
           IOLockSleepDeadline(gExpectLock, &gExpectedTaken, deadline, THREAD_UNINT) != THREAD_TIMED_OUT) {
    }
    const uint32_t taken = gExpectedTaken;
    gExpectedChannel = kChannels;
    IOLockUnlock(gExpectLock);
    const uint32_t pending = gErrorRegisters.read(kHintStatusExt) & bit,
                   stat = gErrorRegisters.read(kCmdprocStat) & bit;
    if (!taken && (pending || stat))
        panic("TegraHost1x: general interrupt (INTID %u) lost: channel %u's illegal opcode latched (HINTSTATUS_EXT %#x "
              "CMDPROC_STAT %#x) and not delivered in %u ms",
              kGeneralIntid, channel, gErrorRegisters.read(kHintStatusExt), gErrorRegisters.read(kCmdprocStat), ms);
    return taken;
}

uint32_t switch_host1x_errors_stat()
{
    return gErrorRegisters.read(kCmdprocStat);
}
#endif
