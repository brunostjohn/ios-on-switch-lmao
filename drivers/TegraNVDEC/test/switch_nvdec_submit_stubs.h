// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <gtest/gtest.h>
#include <cstdint>
#include <new>
#include <cstdlib>
#include <stdexcept>

inline bool switch_test_queue_locked = false;

struct IOLock {
    bool locked = false;
};

inline IOLock *switch_test_queue_lock = nullptr;

inline IOLock *IOLockAlloc()
{
    return new IOLock;
}

inline void IOLockFree(IOLock *lock)
{
    delete lock;
}

inline void IOLockLock(IOLock *lock)
{
    EXPECT_FALSE(lock->locked);
    lock->locked = true;
    if (lock == switch_test_queue_lock)
        switch_test_queue_locked = true;
}

inline void IOLockUnlock(IOLock *lock)
{
    EXPECT_TRUE(lock->locked);
    lock->locked = false;
    if (lock == switch_test_queue_lock)
        switch_test_queue_locked = false;
}

inline void IOLockSleep(IOLock *, void *, int)
{
    throw std::runtime_error("unexpected CPU sleep");
}

inline void IOLockWakeup(IOLock *, void *, bool)
{}

#define IONew(type, count)             static_cast<type *>(std::malloc(sizeof(type) * (count)))
#define IONewZero(type, count)         static_cast<type *>(std::calloc((count), sizeof(type)))
#define IODelete(pointer, type, count) ((void)(count), std::free(pointer))

class IOMemoryDescriptor {};

class IOBufferMemoryDescriptor : public IOMemoryDescriptor {};

using IOOptionBits = uint32_t;
using wait_result_t = int;
using thread_t = void *;
constexpr thread_t THREAD_NULL = nullptr;
constexpr int THREAD_UNINT = 0, KERN_SUCCESS = 0;

inline int kernel_thread_start(void (*)(void *, wait_result_t), void *, thread_t *)
{
    return 0;
}

inline void thread_deallocate(thread_t)
{}

[[noreturn]] inline void panic(const char *, ...)
{
    throw std::runtime_error("panic");
}

constexpr uint64_t NSEC_PER_USEC = 1000, NSEC_PER_MSEC = 1000000;

inline void nanoseconds_to_absolutetime(uint64_t value, uint64_t *out)
{
    *out = value;
}

inline void absolutetime_to_nanoseconds(uint64_t value, uint64_t *out)
{
    *out = value;
}

inline uint64_t mach_absolute_time()
{
    return 1000000;
}

inline void clock_interval_to_deadline(uint32_t value, uint32_t scale, uint64_t *out)
{
    *out = uint64_t(value) * scale;
}
