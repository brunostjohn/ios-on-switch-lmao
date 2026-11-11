// SPDX-License-Identifier: BSD-2-Clause-Patent
// The kernel as TegraHost1x.cpp sees it, for its host test: locks that only track ownership, no sleeps, no interrupts.
#pragma once
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>

using IOReturn = int;
using IOOptionBits = uint32_t;
constexpr IOReturn kIOReturnSuccess = 0;
constexpr int THREAD_UNINT = 0, THREAD_TIMED_OUT = 1;
constexpr uint32_t kMicrosecondScale = 1000, kMillisecondScale = 1000000;
constexpr uint64_t NSEC_PER_USEC = 1000;
constexpr size_t PAGE_SIZE = 16384;

struct IOLock {
    bool held;
};

inline IOLock *IOLockAlloc()
{
    return new IOLock{};
}

inline void IOLockFree(IOLock *lock)
{
    delete lock;
}

inline void IOLockLock(IOLock *lock)
{
    lock->held = true;
}

inline void IOLockUnlock(IOLock *lock)
{
    lock->held = false;
}

inline void IOLockWakeup(IOLock *, void *, bool)
{}

inline int IOLockSleepDeadline(IOLock *, void *, uint64_t, int)
{
    throw std::runtime_error("a submission never sleeps");
}

inline void IOLog(const char *, ...)
{}

inline void IODelay(unsigned)
{}

[[noreturn]] inline void panic(const char *, ...)
{
    throw std::runtime_error("panic");
}

inline uint64_t mach_absolute_time()
{
    return 0;
}

inline void clock_interval_to_deadline(uint32_t interval, uint32_t scale, uint64_t *deadline)
{
    *deadline = uint64_t(interval) * scale;
}

class OSObject {};

class IOService : public OSObject {};

class IOBufferMemoryDescriptor : public OSObject {};

class IOInterruptEventSource : public OSObject {
public:
    using Action = void (*)(OSObject *, IOInterruptEventSource *, int);

    static IOInterruptEventSource *interruptEventSource(OSObject *, Action, IOService *, int)
    {
        static IOInterruptEventSource source;
        return &source;
    }

    void enable()
    {}
};

class IOWorkLoop : public OSObject {
public:
    static IOWorkLoop *workLoop()
    {
        static IOWorkLoop loop;
        return &loop;
    }

    IOReturn addEventSource(IOInterruptEventSource *)
    {
        return kIOReturnSuccess;
    }
};
