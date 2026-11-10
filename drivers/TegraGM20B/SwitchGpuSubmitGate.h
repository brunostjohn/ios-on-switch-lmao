// SPDX-License-Identifier: MIT
#pragma once
#include <IOKit/IOLib.h>
#include <kern/thread.h>

// Every operation is called with the connection lock held. A queued mutation prevents new submissions from
// extending its drain; sleeping releases that lock so other channels, status reads and CPU unlocks can progress.
class SwitchGpuSubmitGate
{
public:
	bool enter(IOLock *lock)
	{
		while (!closed_ && (mutations_ || mutating_))
			IOLockSleep(lock, this, THREAD_UNINT);
		if (closed_)
			return false;
		++active_;
		return true;
	}

	void leave(IOLock *lock)
	{
		if (!--active_)
			IOLockWakeup(lock, this, false);
	}

	bool beginMutation(IOLock *lock)
	{
		++mutations_;
		while (!closed_ && (active_ || mutating_))
			IOLockSleep(lock, this, THREAD_UNINT);
		--mutations_;
		if (closed_) {
			IOLockWakeup(lock, this, false);
			return false;
		}
		mutating_ = true;
		return true;
	}

	void endMutation(IOLock *lock)
	{
		mutating_ = false;
		IOLockWakeup(lock, this, false);
	}

	bool mutate(IOLock *lock)
	{
		if (!beginMutation(lock))
			return false;
		endMutation(lock);
		return true;
	}

	void close(IOLock *lock)
	{
		closed_ = true;
		IOLockWakeup(lock, this, false);
		while (active_ || mutating_)
			IOLockSleep(lock, this, THREAD_UNINT);
	}

	bool closed() const
	{
		return closed_;
	}

private:
	unsigned active_ = 0, mutations_ = 0;
	bool closed_ = false, mutating_ = false;
};
