// SPDX-License-Identifier: MIT
#pragma once
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
