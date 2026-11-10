// SPDX-License-Identifier: MIT
#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
struct IOLock { std::mutex mutex; std::condition_variable changed; };
inline IOLock *IOLockAlloc() { return new IOLock; }
inline void IOLockFree(IOLock *lock) { delete lock; }
inline void IOLockLock(IOLock *lock) __attribute__((no_thread_safety_analysis)) { lock->mutex.lock(); }
inline void IOLockUnlock(IOLock *lock) __attribute__((no_thread_safety_analysis)) { lock->mutex.unlock(); }
inline void IOLockSleep(IOLock *lock, void *, int) __attribute__((no_thread_safety_analysis)) {
    std::unique_lock held(lock->mutex, std::adopt_lock);
    lock->changed.wait(held);
    held.release();
}
inline void IOLockWakeup(IOLock *lock, void *, bool) { lock->changed.notify_all(); }
constexpr int THREAD_UNINT = 0, THREAD_CALL_PRIORITY_KERNEL = 0, THREAD_CALL_OPTIONS_ONCE = 1;
constexpr int THREAD_AWAKENED = 0, THREAD_TIMED_OUT = 1;
inline void clock_interval_to_deadline(uint32_t interval, uint32_t scale, uint64_t *deadline)
{
    *deadline = uint64_t(interval) * scale;
}
// One host millisecond stands for each deadline, so a stalled bridge reaches its hang check at once.
inline int IOLockSleepDeadline(IOLock *lock, void *, uint64_t, int) __attribute__((no_thread_safety_analysis))
{
    std::unique_lock held(lock->mutex, std::adopt_lock);
    const bool woken = lock->changed.wait_for(held, std::chrono::milliseconds(1)) == std::cv_status::no_timeout;
    held.release();
    return woken ? THREAD_AWAKENED : THREAD_TIMED_OUT;
}
using thread_call_param_t = void *;
struct SwitchBridgeCall { void (*fn)(void *, void *); void *arg; bool pending; };
using thread_call_t = SwitchBridgeCall *;
extern thread_call_t switch_bridge_call;
inline thread_call_t thread_call_allocate_with_options(void (*fn)(void *, void *), void *arg, int, int) {
    switch_bridge_call = new SwitchBridgeCall{fn, arg, false};
    return switch_bridge_call;
}
inline void thread_call_free(thread_call_t call) { delete call; }
inline void thread_call_enter(thread_call_t call) { call->pending = true; }

[[noreturn]] void panic(const char *, ...);
inline unsigned switch_bridge_logs;

inline void IOLog(const char *, ...)
{
	++switch_bridge_logs;
}
