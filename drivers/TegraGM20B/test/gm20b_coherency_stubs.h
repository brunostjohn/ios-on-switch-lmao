// SPDX-License-Identifier: MIT
// The kernel calls TegraGM20BCoherency.cpp and TegraGM20BExecSurfaces.cpp make, on the host: IOLock as a real mutex (the
// concurrency test runs traps against releases on two threads), IONew/IODelete, IOLog counted, and tgm_cpu_clean_pages
// recording each call's pages.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

class IOUserClient
{
public:
	bool inactive = false;
	unsigned refs = 1;

	bool isInactive() const
	{
		return inactive;
	}

	void retain()
	{
		++refs;
	}

	void release()
	{
		--refs;
	}
};

struct IOLock {
	std::mutex m;
	std::condition_variable condition;
	std::atomic<std::thread::id> owner{};
	uint64_t takes = 0;  // under m
};

// Every IOLockLock of the process
inline std::atomic<uint64_t> &ioLockTakes()
{
	static std::atomic<uint64_t> takes;
	return takes;
}

inline IOLock *IOLockAlloc()
{
	return new IOLock;
}

// The CPU a thread runs on (a test may move it), and its preemption level, which no IOLock may be taken under
inline int &hostCpu()
{
	static thread_local int cpu;
	return cpu;
}

inline int &preemptionOff()
{
	static thread_local int level;
	return level;
}

extern "C" inline int cpu_number(void)
{
	return hostCpu();
}

extern "C" inline void _disable_preemption(void)
{
	++preemptionOff();
}

extern "C" inline void _enable_preemption(void)
{
	--preemptionOff();
}

inline void IOLockFree(IOLock *lock)
{
	delete lock;
}

// XNU's IOLock (lck_mtx) is not recursive: a thread taking one it holds never returns. The model aborts there.
inline void IOLockLock(IOLock *lock) __attribute__((no_thread_safety_analysis))
{
	if (lock->owner.load(std::memory_order_relaxed) == std::this_thread::get_id()) {
		std::fputs("IOLockLock: the thread already holds this lock\n", stderr);
		std::abort();
	}
	if (preemptionOff()) {
		std::fputs("IOLockLock: preemption is off\n", stderr);
		std::abort();
	}
	lock->m.lock();
	lock->owner.store(std::this_thread::get_id(), std::memory_order_relaxed);
	++lock->takes;
	ioLockTakes().fetch_add(1, std::memory_order_relaxed);
}

inline void IOLockUnlock(IOLock *lock) __attribute__((no_thread_safety_analysis))
{
	lock->owner.store(std::thread::id(), std::memory_order_relaxed);
	lock->m.unlock();
}

inline void IOLockSleep(IOLock *lock, void *, int) __attribute__((no_thread_safety_analysis))
{
	std::unique_lock<std::mutex> held(lock->m, std::adopt_lock);
	lock->owner.store(std::thread::id(), std::memory_order_relaxed);
	lock->condition.wait(held);
	lock->owner.store(std::this_thread::get_id(), std::memory_order_relaxed);
	held.release();
}

inline void IOLockWakeup(IOLock *lock, void *, bool)
{
	lock->condition.notify_all();
}

#define THREAD_UNINT 0

inline void *current_thread()
{
	static thread_local int identity;
	return &identity;
}

inline void *current_task()
{
	return current_thread();
}

inline bool &failZeroAllocation()
{
	static bool fail = false;
	return fail;
}

#define IONewZero(type, count) (failZeroAllocation() ? nullptr : new type[(count)]{})

inline bool &failAllocation()
{
	static bool fail = false;
	return fail;
}

#define IONew(type, count) (failAllocation() ? nullptr : new type[(count)])
#define IODelete(ptr, type, count) (delete[] (ptr))

inline void bzero(void *p, size_t n)
{
	std::memset(p, 0, n);
}

// IOLog's lines, counted (TegraGM20BExecSurfaces.cpp's one-shot line).
inline int &ioLogLines()
{
	static int lines;
	return lines;
}

inline void IOLog(const char *, ...)
{
	__atomic_fetch_add(&ioLogLines(), 1, __ATOMIC_RELAXED);
}

// Every tgm_cpu_clean_pages call: its page list.
std::vector<std::vector<uint64_t>> &cleanCalls();
