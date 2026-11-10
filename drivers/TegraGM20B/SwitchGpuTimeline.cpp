// SPDX-License-Identifier: MIT
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include "SwitchGpuTimeline.h"
#include "TegraGM20BGpu.h"

static_assert(__atomic_always_lock_free(sizeof(uint64_t), nullptr));

struct switch_gpu_timeline {
	switch_gpu_timeline *next;
	uint64_t uid;
	const volatile uint32_t *fence;
	const volatile uint32_t *allocation;
	uint64_t terminal;
	uint32_t refs;
};

namespace {
IOLock *lock;
switch_gpu_timeline *timelines;

bool initialize()
{
	if (__atomic_load_n(&lock, __ATOMIC_ACQUIRE))
		return true;
	IOLock *made = IOLockAlloc();
	if (!made)
		return false;
	IOLock *expected = nullptr;
	if (!__atomic_compare_exchange_n(&lock, &expected, made, false, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE))
		IOLockFree(made);
	return true;
}

switch_gpu_timeline *find(const switch_gpu_backing_token *token)
{
	if (!token || token->engine != SWITCH_GPU_ENGINE_GM20B || !token->reset_generation ||
	    token->reset_generation > UINT32_MAX || token->value > UINT32_MAX)
		return nullptr;
	const uint64_t uid = ((token->reset_generation - 1) << 32) | token->queue;
	for (auto *at = timelines; at; at = at->next)
		if (at->uid == uid)
			return at;
	return nullptr;
}

int status(const switch_gpu_backing_token *token)
{
	IOLockLock(lock);
	const auto *timeline = find(token);
	int result = -19;
	if (timeline) {
		uint64_t terminal = __atomic_load_n(&timeline->terminal, __ATOMIC_ACQUIRE);
		uint32_t completed = uint32_t(terminal);
		if (!terminal && timeline->fence) {
			completed = __atomic_load_n(timeline->fence, __ATOMIC_ACQUIRE);
			// Recovery publishes failure before advancing the abandoned fence to release hardware acquires.
			terminal = __atomic_load_n(&timeline->terminal, __ATOMIC_ACQUIRE);
			if (terminal)
				completed = uint32_t(terminal);
		}
		result = int32_t(completed - uint32_t(token->value)) >= 0 ? 1 :
			(terminal || !timeline->fence ? -5 : 0);
	}
	IOLockUnlock(lock);
	return result;
}

int wait(const switch_gpu_backing_token *token)
{
	int result = status(token);
	if (!result) {
		tgm_writer_wait(token->queue, uint32_t(token->value));
		result = status(token);
	}
	return result;
}

int retain(const switch_gpu_backing_token *token)
{
	IOLockLock(lock);
	auto *timeline = find(token);
	if (timeline)
		++timeline->refs;
	IOLockUnlock(lock);
	return timeline ? 0 : -19;
}

void put_locked(switch_gpu_timeline *timeline)
{
	if (--timeline->refs)
		return;
	auto **at = &timelines;
	while (*at && *at != timeline)
		at = &(*at)->next;
	if (*at)
		*at = timeline->next;
	switch_gpu_fence_put(timeline->allocation);
	IODelete(timeline, switch_gpu_timeline, 1);
}

void release(const switch_gpu_backing_token *token)
{
	IOLockLock(lock);
	if (auto *timeline = find(token))
		put_locked(timeline);
	IOLockUnlock(lock);
}
} // namespace

extern "C" int switch_gpu_timeline_init(void)
{
	if (!initialize())
		return -12;
	const switch_gpu_backing_engine operations = {status, wait, retain, release, 32};
	const int result = switch_gpu_backing_register_engine(SWITCH_GPU_ENGINE_GM20B, &operations);
	return result == -16 ? 0 : result;
}

extern "C" switch_gpu_timeline *switch_gpu_timeline_create(uint64_t uid, const volatile uint32_t *fence)
{
	if (!fence || !lock)
		return nullptr;
	auto *timeline = IONew(switch_gpu_timeline, 1);
	if (!timeline)
		return nullptr;
	IOLockLock(lock);
	switch_gpu_fence_hold(fence);
	*timeline = {timelines, uid, fence, fence, 0, 1};
	timelines = timeline;
	IOLockUnlock(lock);
	return timeline;
}

extern "C" void switch_gpu_timeline_failed(switch_gpu_timeline *timeline, uint32_t completed)
{
	if (!timeline)
		return;
	uint64_t expected = 0;
	__atomic_compare_exchange_n(&timeline->terminal, &expected, (uint64_t(1) << 32) | completed,
		false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}

extern "C" void switch_gpu_timeline_close(switch_gpu_timeline *timeline)
{
	if (!timeline)
		return;
	IOLockLock(lock);
	uint64_t expected = 0;
	const uint32_t completed = __atomic_load_n(timeline->fence, __ATOMIC_ACQUIRE);
	__atomic_compare_exchange_n(&timeline->terminal, &expected, (uint64_t(1) << 32) | completed,
		false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
	timeline->fence = nullptr;
	put_locked(timeline);
	IOLockUnlock(lock);
}

extern "C" void switch_gpu_timeline_token(switch_gpu_timeline *timeline, uint32_t value,
		switch_gpu_backing_token *token)
{
	*token = {value, (timeline->uid >> 32) + 1, SWITCH_GPU_ENGINE_GM20B, uint32_t(timeline->uid)};
}
