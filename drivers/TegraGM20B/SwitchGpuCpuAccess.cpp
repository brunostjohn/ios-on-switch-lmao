// SPDX-License-Identifier: MIT
#include <kern/thread.h>
#include "SwitchGpuBackingInternal.h"

namespace {
constexpr uint32_t kWrite = SWITCH_GPU_BACKING_WRITE;
constexpr uint32_t kRead = SWITCH_GPU_BACKING_READ;
constexpr uint32_t kAvoid = SWITCH_GPU_BACKING_CPU_AVOID_SYNC;
constexpr uint32_t kDiscard = SWITCH_GPU_BACKING_CPU_DISCARD;
constexpr uint32_t kNoCache = SWITCH_GPU_BACKING_CPU_NO_CACHE;
constexpr uint32_t kConcurrent = SWITCH_GPU_BACKING_CPU_CONCURRENT;

bool valid_range(switch_gpu_backing *backing, uint64_t offset, uint64_t bytes, uint32_t access)
{
	const uint64_t size = uint64_t(backing->pages->count) << 12;
	return (access & (kRead | kWrite)) && !(access & ~(kRead | kWrite | kAvoid | kDiscard | kNoCache | kConcurrent)) && bytes &&
		offset <= size && bytes <= size - offset &&
		(!(access & kConcurrent) || (!offset && bytes == size)) &&
		(!(access & kDiscard) || ((access & kWrite) && !(access & kRead) && !((offset | bytes) & 63)));
}

bool conflicts(uint32_t recorded, uint32_t access)
{
	return (recorded | access) & kWrite;
}

int wait_locked(switch_gpu_backing *backing, uint32_t access)
{
	for (;;) {
		int result = switch_gpu_backing_prune_locked(backing);
		if (result)
			return result;
		switch_gpu_backing_token token = {};
		for (uint32_t i = 0; i < backing->access_count; ++i) {
			if (conflicts(backing->accesses[i].access, access) &&
			    switch_gpu_backing_token_status(&backing->accesses[i].token) == 0) {
				token = backing->accesses[i].token;
				break;
			}
		}
		if (!token.engine)
			return 0;
		if (access & kAvoid)
			return -16;
		const auto *ops = switch_gpu_backing_engine_ops(token.engine);
		result = ops->retain_token(&token);
		if (result)
			return result;
		IOLockUnlock(backing->lock);
		result = ops->wait(&token);
		ops->release_token(&token);
		IOLockLock(backing->lock);
		if (result != 1)
			return result < 0 ? result : -110;
	}
}

void release_cpu_locked(switch_gpu_backing *backing, uint32_t access)
{
	if (access & kWrite)
		--backing->cpu_writers;
	else
		--backing->cpu_readers;
	IOLockWakeup(backing->lock, &backing->cpu_writers, false);
}
} // namespace

extern "C" int switch_gpu_backing_cpu_acquire(switch_gpu_backing *backing, uint64_t offset,
		uint64_t bytes, uint32_t access)
{
	if (!backing)
		return -22;
	switch_gpu_backing_retain(backing);
	IOLockLock(backing->lock);
	int result = valid_range(backing, offset, bytes, access) ? 0 : -22;
	while (!result && !(access & kConcurrent) && (backing->cpu_writers || ((access & kWrite) && backing->cpu_readers))) {
		if (access & kAvoid)
			result = -16;
		else
			IOLockSleep(backing->lock, &backing->cpu_writers, THREAD_UNINT);
	}
	bool held = false;
	if (!result) {
		++((access & kWrite) ? backing->cpu_writers : backing->cpu_readers);
		held = true;
		result = wait_locked(backing, access);
	}
	if (!result) {
		const uint64_t size = uint64_t(backing->pages->count) << 12;
		if ((backing->dirty & SWITCH_GPU_BACKING_GPU_DIRTY) && !(access & (kDiscard | kNoCache)))
			switch_gpu_backing_cache(backing->pages->addresses, backing->pages->count, offset, bytes, 1);
		if (!(access & kNoCache) && !offset && bytes == size)
			backing->dirty &= ~uint32_t(SWITCH_GPU_BACKING_GPU_DIRTY);
		if (access & kWrite)
			backing->dirty |= SWITCH_GPU_BACKING_CPU_DIRTY;
	} else if (held) {
		release_cpu_locked(backing, access);
	}
	IOLockUnlock(backing->lock);
	if (result)
		switch_gpu_backing_release(backing);
	return result;
}

extern "C" int switch_gpu_backing_cpu_release(switch_gpu_backing *backing, uint64_t offset,
		uint64_t bytes, uint32_t access)
{
	if (!backing)
		return -22;
	IOLockLock(backing->lock);
	if (!valid_range(backing, offset, bytes, access) ||
	    ((access & kWrite) ? !backing->cpu_writers : !backing->cpu_readers)) {
		IOLockUnlock(backing->lock);
		return -22;
	}
	if (access & kWrite) {
		if (!(access & kNoCache))
			switch_gpu_backing_cache(backing->pages->addresses, backing->pages->count, offset, bytes, 0);
		++backing->content;
		if (backing->initial_clean && backing->cpu_writers == 1)
			backing->dirty &= ~uint32_t(SWITCH_GPU_BACKING_CPU_DIRTY);
	}
	release_cpu_locked(backing, access);
	IOLockUnlock(backing->lock);
	switch_gpu_backing_release(backing);
	return 0;
}

extern "C" int switch_gpu_backing_wait(switch_gpu_backing *backing, uint32_t access)
{
	if (!backing || !(access & (kRead | kWrite)) || (access & ~(kRead | kWrite | kAvoid)))
		return -22;
	IOLockLock(backing->lock);
	const int result = wait_locked(backing, access);
	IOLockUnlock(backing->lock);
	return result;
}

extern "C" int switch_gpu_backing_cpu_publish(switch_gpu_backing *backing, uint64_t offset, uint64_t bytes)
{
	if (!backing)
		return -22;
	IOLockLock(backing->lock);
	const int result = !backing->cpu_writers || !valid_range(backing, offset, bytes, kWrite) ? -22 : 0;
	if (!result)
		switch_gpu_backing_cache(backing->pages->addresses, backing->pages->count, offset, bytes, 0);
	IOLockUnlock(backing->lock);
	return result;
}

extern "C" void switch_gpu_backing_cpu_cancel(switch_gpu_backing *backing, uint32_t access)
{
	if (!backing)
		return;
	IOLockLock(backing->lock);
	if ((access & kWrite) && backing->initial_clean && backing->cpu_writers == 1)
		backing->dirty &= ~uint32_t(SWITCH_GPU_BACKING_CPU_DIRTY);
	release_cpu_locked(backing, access);
	IOLockUnlock(backing->lock);
	switch_gpu_backing_release(backing);
}
