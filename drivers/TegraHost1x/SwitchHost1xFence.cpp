// SPDX-License-Identifier: BSD-2-Clause-Patent
// The fence lists in the kernel: one lock, the syncpoint interrupt's action, the work-loop hand-off and one timer for
// the bounds. See SwitchHost1xFence.h.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include "SwitchHost1xFence.h"
#include "SwitchHost1xFenceList.h"
#include "SwitchHost1xIntr.h"
#include "TegraHost1x.h"
#include "../ArmGIC/SwitchInterrupts.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"

namespace {
constexpr uint32_t kGraceMs = 50;

struct Registers {
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

IOLock *gLock;  // the lists, the hand-off, the timer's deadline
Registers gRegisters;
SwitchHost1xFenceList<Registers> gList;
SwitchHost1xExpired gHandOff;
IOInterruptEventSource *gKick;
thread_call_t gBounds;
uint64_t gBoundsAt;
uint32_t gIntid;
bool gUp, gStrict;

uint64_t deadlineIn(uint32_t us)
{
    uint64_t deadline = 0;
    clock_interval_to_deadline(us, kMicrosecondScale, &deadline);
    return deadline;
}

void armBoundsLocked(uint64_t deadline)
{
    if (gBoundsAt && gBoundsAt <= deadline)
        return;
    gBoundsAt = deadline;
    thread_call_enter_delayed(gBounds, deadline);
}

// Waiters (no callback) are woken here, under the lock; fences with a callback move to `callbacks` in state.
void sortLocked(SwitchHost1xExpired &done, SwitchHost1xExpired &callbacks, uint8_t state)
{
    for (auto *fence = done.take(); fence;) {
        auto *next = fence->next;
        if (fence->signaled) {
            fence->state = state;
            callbacks.push(fence);
        } else {
            fence->state = SWITCH_HOST1X_FENCE_IDLE;
            IOLockWakeup(gLock, fence, false);
        }
        fence = next;
    }
}

// Outside the work loop, callbacks are handed to it.
void settleLocked(SwitchHost1xExpired &done)
{
    const bool idle = !gHandOff.first;
    sortLocked(done, gHandOff, SWITCH_HOST1X_FENCE_HANDED_OFF);
    if (idle && gHandOff.first)
        gKick->interruptOccurred(nullptr, nullptr, 0);
}

[[noreturn]] void panicLost(const switch_host1x_fence *fence)
{
    char gic[192];
    switch_interrupt_describe(gIntid, gic, sizeof(gic));
    const uint32_t id = fence->syncpt;
    panic("TegraHost1x: syncpoint %u threshold interrupt lost (INTID %u): value %u threshold %u INT_STATUS[%u] %#x, %s",
          id, gIntid, gList.value(gRegisters, id), fence->threshold, id / 32, gList.status(gRegisters, id), gic);
}

void kicked(OSObject *, IOInterruptEventSource *, int)
{
    switch_host1x_intr_handle();
}

void bounds(thread_call_param_t, thread_call_param_t)
{
    SwitchHost1xExpired done;
    uint64_t grace = 0, next = 0;
    nanoseconds_to_absolutetime(static_cast<uint64_t>(kGraceMs) * NSEC_PER_MSEC, &grace);
    IOLockLock(gLock);
    gBoundsAt = 0;
    if (gUp) {
        if (const auto *lost = gList.overdue(gRegisters, mach_absolute_time(), grace, gStrict, TEGRA_HOST1X_ETIMEDOUT,
                                             TEGRA_HOST1X_ELOST, done, &next))
            panicLost(lost);
        if (next)
            armBoundsLocked(next);
    }
    settleLocked(done);
    IOLockUnlock(gLock);
}
}  // namespace

bool switch_host1x_intr_init(IOWorkLoop *loop, volatile uint8_t *sync, uint32_t intid)
{
    gRegisters.sync = sync;
    gIntid = intid;
    gBounds = thread_call_allocate_with_options(bounds, nullptr, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
    gKick = IOInterruptEventSource::interruptEventSource(loop, &kicked);
    IOLock *lock = IOLockAlloc();
    if (!lock || !gBounds || !gKick || loop->addEventSource(gKick) != kIOReturnSuccess)
        return false;
    gKick->enable();
    __atomic_store_n(&gLock, lock, __ATOMIC_RELEASE);
    return true;
}

void switch_host1x_intr_handle()
{
    SwitchHost1xExpired callbacks, done;
    IOLockLock(gLock);
    done.append(gHandOff);
    if (gUp)
        gList.interrupt(gRegisters, done);
    sortLocked(done, callbacks, SWITCH_HOST1X_FENCE_DELIVERING);
    IOLockUnlock(gLock);
    for (auto *fence = callbacks.take(); fence;) {
        auto *next = fence->next;
        auto *signaled = fence->signaled;
        const int status = fence->status;
        __atomic_store_n(&fence->state, SWITCH_HOST1X_FENCE_IDLE, __ATOMIC_RELEASE);
        signaled(fence, status);
        fence = next;
    }
}

void switch_host1x_intr_stop()
{
    IOLockLock(gLock);
    gUp = false;
    IOLockUnlock(gLock);
}

void switch_host1x_intr_start()
{
    SwitchHost1xExpired done;
    IOLockLock(gLock);
    gUp = true;
    gList.rearm(gRegisters, done);
    settleLocked(done);
    IOLockUnlock(gLock);
}

extern "C" void switch_host1x_fence_init(switch_host1x_fence *fence, uint32_t syncpt, uint32_t threshold,
                                         void (*signaled)(switch_host1x_fence *, int))
{
    *fence = {};
    fence->syncpt = syncpt;
    fence->threshold = threshold;
    fence->signaled = signaled;
}

extern "C" int switch_host1x_fence_add(switch_host1x_fence *fence, uint32_t boundUs)
{
    if (!gLock)
        return TEGRA_HOST1X_ENODEV;
    if (!fence || fence->syncpt >= SwitchHost1xFenceList<Registers>::kSyncpts ||
        __atomic_load_n(&fence->state, __ATOMIC_ACQUIRE) != SWITCH_HOST1X_FENCE_IDLE)
        return TEGRA_HOST1X_EINVAL;
    SwitchHost1xExpired done;
    IOLockLock(gLock);
    if (!gUp) {
        IOLockUnlock(gLock);
        return TEGRA_HOST1X_ENODEV;
    }
    fence->graced = 0;
    fence->deadline = boundUs ? deadlineIn(boundUs) : 0;
    gList.add(gRegisters, fence, done);
    if (fence->state == SWITCH_HOST1X_FENCE_QUEUED && fence->deadline)
        armBoundsLocked(fence->deadline);
    settleLocked(done);
    IOLockUnlock(gLock);
    return TEGRA_HOST1X_OK;
}

extern "C" void switch_host1x_fence_watch(switch_host1x_fence *fence, uint32_t boundUs)
{
    if (!gLock || !fence)
        return;
    IOLockLock(gLock);
    if (fence->state == SWITCH_HOST1X_FENCE_QUEUED) {
        fence->graced = 0;
        fence->deadline = deadlineIn(boundUs);
        armBoundsLocked(fence->deadline);
    }
    IOLockUnlock(gLock);
}

extern "C" bool switch_host1x_fence_remove(switch_host1x_fence *fence)
{
    if (!gLock || !fence)
        return false;
    SwitchHost1xExpired done;
    IOLockLock(gLock);
    bool removed = gUp && gList.remove(gRegisters, fence, done);
    if (!removed && fence->state == SWITCH_HOST1X_FENCE_HANDED_OFF && gHandOff.erase(fence)) {
        fence->state = SWITCH_HOST1X_FENCE_IDLE;
        removed = true;
    }
    settleLocked(done);
    IOLockUnlock(gLock);
    return removed;
}

// A waiter is a fence without a callback, woken under the lock. A wait that ends with the syncpoint past the threshold
// but its fence still queued, after a grace period for an action under way, means the interrupt never reached the CPU:
// a panic once strict (an engine was proven and published), TEGRA_HOST1X_ELOST while its probe is still running.
extern "C" int tegra_host1x_syncpt_wait(uint32_t id, uint32_t threshold, uint32_t timeoutUs)
{
    if (!gLock)
        return TEGRA_HOST1X_ENODEV;
    if (id >= SwitchHost1xFenceList<Registers>::kSyncpts)
        return TEGRA_HOST1X_EINVAL;
    switch_host1x_fence waiter;
    switch_host1x_fence_init(&waiter, id, threshold, nullptr);
    SwitchHost1xExpired done;
    IOLockLock(gLock);
    if (!gUp) {
        IOLockUnlock(gLock);
        return TEGRA_HOST1X_ENODEV;
    }
    gList.add(gRegisters, &waiter, done);
    settleLocked(done);
    uint64_t deadline = deadlineIn(timeoutUs);
    int ret = TEGRA_HOST1X_OK;
    for (bool grace = false; waiter.state == SWITCH_HOST1X_FENCE_QUEUED;) {
        if (IOLockSleepDeadline(gLock, &waiter, deadline, THREAD_UNINT) != THREAD_TIMED_OUT)
            continue;
        if (waiter.state != SWITCH_HOST1X_FENCE_QUEUED)
            break;
        if (!switch_host1x_reached(gList.value(gRegisters, id), threshold)) {
            ret = TEGRA_HOST1X_ETIMEDOUT;  // the engine did not get there: its failure, not the interrupt's
            break;
        }
        if (grace) {
            if (!__atomic_load_n(&gStrict, __ATOMIC_ACQUIRE)) {
                ret = TEGRA_HOST1X_ELOST;
                break;
            }
            panicLost(&waiter);
        }
        grace = true;
        clock_interval_to_deadline(kGraceMs, kMillisecondScale, &deadline);
    }
    gList.remove(gRegisters, &waiter, done);
    settleLocked(done);
    IOLockUnlock(gLock);
    return ret;
}

extern "C" void tegra_host1x_set_strict(void)
{
    __atomic_store_n(&gStrict, true, __ATOMIC_RELEASE);
}
