// SPDX-License-Identifier: MIT
#pragma once
#include <cstdlib>
#include <mutex>

inline thread_local bool switchTimelineAtomicContext;

inline void switchTimelineMayBlock()
{
	if (switchTimelineAtomicContext)
		std::abort();
}

struct IOLock {
	std::mutex mutex;
};

inline IOLock *IOLockAlloc()
{
	switchTimelineMayBlock();
	return new IOLock;
}

inline void IOLockFree(IOLock *lock)
{
	switchTimelineMayBlock();
	delete lock;
}

inline void IOLockLock(IOLock *lock) __attribute__((no_thread_safety_analysis))
{
	switchTimelineMayBlock();
	lock->mutex.lock();
}

inline void IOLockUnlock(IOLock *lock) __attribute__((no_thread_safety_analysis))
{
	lock->mutex.unlock();
}

template <class T> T *switchTimelineNew(unsigned long count)
{
	switchTimelineMayBlock();
	return new T[count];
}

template <class T> void switchTimelineDelete(T *value)
{
	switchTimelineMayBlock();
	delete[] value;
}

#define IONew(type, count) switchTimelineNew<type>(count)
#define IODelete(value, type, count) switchTimelineDelete(value)
