// SPDX-License-Identifier: MIT
// The kernel with interrupts off, as local_irq_save leaves it. XNU panics on any mutex, sleep or allocation then
// ("Attempt to take mutex ... with preemption disabled"), so every stand-in for one throws while they are off.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace interrupts_off
{
inline int depth = 0;
inline unsigned saves = 0;
// A word read at each save and restore: where a store to it fell.
inline const volatile uint32_t *watched = nullptr;
inline uint32_t atSave = 0, atRestore = 0;

inline void mayBlock(const char *what)
{
	if (depth)
		throw std::runtime_error(std::string(what) + " with preemption disabled");
}
}  // namespace interrupts_off

inline unsigned long switch_linux_irq_save()
{
	++interrupts_off::saves;
	if (interrupts_off::watched)
		interrupts_off::atSave = *interrupts_off::watched;
	return interrupts_off::depth++ == 0;
}

inline void switch_linux_irq_restore(unsigned long)
{
	if (interrupts_off::watched)
		interrupts_off::atRestore = *interrupts_off::watched;
	--interrupts_off::depth;
}

#define local_irq_save(f) ((f) = switch_linux_irq_save())
#define local_irq_restore(f) switch_linux_irq_restore(f)

struct IOLock {};

struct IORecursiveLock {};

inline void IOLockLock(IOLock *)
{
	interrupts_off::mayBlock("IOLockLock");
}

inline void IORecursiveLockLock(IORecursiveLock *)
{
	interrupts_off::mayBlock("IORecursiveLockLock");
}

inline void *IOMalloc(size_t size)
{
	interrupts_off::mayBlock("IOMalloc");
	return std::malloc(size);
}

__attribute__((format(printf, 1, 2))) inline void IOLog(const char *, ...)
{
	interrupts_off::mayBlock("IOLog");
}

struct IOCommandGate {
	template <class... Args> int runAction(Args...)
	{
		interrupts_off::mayBlock("IOCommandGate::runAction");
		return 0;
	}
};

// Kernel C reaches the same through the shim (nvif/os.h, nvxnu.h).
struct mutex {
	void *lck;
};

inline void mutex_lock(struct mutex *)
{
	interrupts_off::mayBlock("mutex_lock (lck_mtx_lock)");
}

inline void *kmalloc(size_t size, unsigned)
{
	interrupts_off::mayBlock("kmalloc (IOMalloc)");
	return std::malloc(size);
}

inline void *kzalloc(size_t size, unsigned)
{
	interrupts_off::mayBlock("kzalloc (IOMalloc)");
	return std::calloc(1, size);
}

__attribute__((format(printf, 1, 2))) inline void nvxnu_log(const char *, ...)
{
	interrupts_off::mayBlock("nvxnu_log (IOLog)");
}
