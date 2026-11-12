// SPDX-License-Identifier: BSD-2-Clause-Patent
// The kernel as TegraHost1x.cpp and SwitchHost1xFence.cpp see it, for the fence test: a sleep runs the test's hook in
// place of the scheduler, the hand-off source and the bound timer record what they were asked to do.
#pragma once
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <stdexcept>

using IOReturn = int;
using IOOptionBits = uint32_t;
constexpr IOReturn kIOReturnSuccess = 0;
constexpr int THREAD_UNINT = 0, THREAD_AWAKENED = 0, THREAD_TIMED_OUT = 1;
constexpr uint32_t kMicrosecondScale = 1000, kMillisecondScale = 1000000;
constexpr uint64_t NSEC_PER_USEC = 1000, NSEC_PER_MSEC = 1000000;
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
    if (lock->held)
        throw std::runtime_error("lock taken twice");
    lock->held = true;
}

inline void IOLockUnlock(IOLock *lock)
{
    lock->held = false;
}

inline uint32_t switch_test_wakeups;

inline void IOLockWakeup(IOLock *, void *, bool)
{
    ++switch_test_wakeups;
}

// Called with the lock held and the deadline: returns THREAD_AWAKENED or THREAD_TIMED_OUT.
inline std::function<int(uint64_t deadline)> switch_test_sleep;

inline int IOLockSleepDeadline(IOLock *lock, void *, uint64_t deadline, int)
{
    if (!switch_test_sleep)
        throw std::runtime_error("unexpected sleep");
    lock->held = false;
    const int result = switch_test_sleep(deadline);
    lock->held = true;
    return result;
}

inline void IOLog(const char *, ...)
{}

inline void IODelay(unsigned)
{}

[[noreturn]] inline void panic(const char *format, ...)
{
    char text[512];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    throw std::runtime_error(text);
}

inline uint64_t switch_test_now;

inline uint64_t mach_absolute_time()
{
    return switch_test_now;
}

inline void clock_interval_to_deadline(uint32_t interval, uint32_t scale, uint64_t *deadline)
{
    *deadline = switch_test_now + uint64_t(interval) * scale;
}

inline void nanoseconds_to_absolutetime(uint64_t nanoseconds, uint64_t *out)
{
    *out = nanoseconds;
}

class OSObject {};

class IOService : public OSObject {};

class IOBufferMemoryDescriptor : public OSObject {};

inline uint32_t switch_test_kicks;

class IOInterruptEventSource : public OSObject {
public:
    using Action = void (*)(OSObject *, IOInterruptEventSource *, int);

    static IOInterruptEventSource *interruptEventSource(OSObject *, Action, IOService * = nullptr, int = 0)
    {
        return new IOInterruptEventSource;
    }

    void enable()
    {}

    void interruptOccurred(void *, IOService *, int)
    {
        ++switch_test_kicks;
    }
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

using thread_call_param_t = void *;
using thread_call_func_t = void (*)(thread_call_param_t, thread_call_param_t);
using thread_call_t = thread_call_func_t *;
constexpr int THREAD_CALL_PRIORITY_KERNEL = 0, THREAD_CALL_OPTIONS_ONCE = 1;
inline thread_call_func_t switch_test_bounds;
inline uint64_t switch_test_bounds_at;

inline thread_call_t thread_call_allocate_with_options(thread_call_func_t function, thread_call_param_t, int, int)
{
    switch_test_bounds = function;
    return &switch_test_bounds;
}

inline bool thread_call_enter_delayed(thread_call_t, uint64_t deadline)
{
    switch_test_bounds_at = deadline;
    return false;
}
