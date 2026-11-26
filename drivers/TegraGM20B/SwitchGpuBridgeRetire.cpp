// SPDX-License-Identifier: MIT
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include "SwitchGpuBridgeQueue.h"
#include "SwitchGpuBridgeProducer.h"
#include "SwitchGpuTimeline.h"
#include "TegraGM20BGpu.h"
#include "TegraGM20BSyncpt.h"
#include "../TegraHost1x/TegraHost1x.h"

namespace switch_gpu_bridge_queue
{
namespace
{
switch_gpu_bridge *oldestWaitedJob()
{
	switch_gpu_bridge *oldest = nullptr;
	for (auto &job : queue.jobs)
		if (job.active && job.waiters && !job.producer &&
		    (!oldest || int32_t(job.sequence - oldest->sequence) < 0))
			oldest = &job;
	return oldest;
}

// A jobless slot by its covering job's channel: done once that job drained or the channel stopped short of it. Only a
// channel that stopped short of it owes increments: one whose covering fence landed issued them first (gpu_exec.c
// chan_fence_job), and a token that failed elsewhere leaves a running producer's syncpoint alone. The tokens are read
// before the channel, so a kill that failed one of them is already in `stopped`.
bool settleJobless(switch_gpu_bridge &job)
{
	const int source = producers(job);
	int stopped = 0;
	const int cover = switch_gpu_producer_covered(job.covering, &stopped);
	if (source >= 0 && (cover == 0 || source == 0))
		return false;
	job.result = source < 0 ? source : cover == 1 ? 1 : -5;
	if (stopped && cover != 1)
		switch_gpu_producer_release(job.producer, job.threshold);
	return true;
}
}  // namespace

bool anyWaited()
{
	for (const auto &job : queue.jobs)
		if (job.active && job.waiters)
			return true;
	return false;
}

void armHangCheck(uint64_t ns)
{
	clock_interval_to_deadline(uint32_t(ns / 1000), 1000, &queue.checkAt);
	thread_call_enter_delayed(queue.hangCheck, queue.checkAt);
}

// The GPU's hang recovery runs only when something waits on a channel, and a wait on a bridge is that waiter. A wait on
// a bridge job has recovery look at the bridge channel, at what its oldest waited job is stuck on, whose kill releases
// the bridge's acquire; a wait on a jobless bridge waits on its producer's covering job, so each is checked on that
// channel itself. One timer does it, armed when a wait begins, cancelled when no waited bridge is left (retire), and
// re-armed for the soonest next step of tgm_chan_check_progress's rule. The references keep the slots across the
// unlocked checks.
void checkHang(thread_call_param_t, thread_call_param_t)
{
	IOLockLock(queue.lock);
	queue.checkAt = 0;
	switch_gpu_bridge *late = queue.space.channel ? oldestWaitedJob() : nullptr;
	switch_gpu_bridge *jobless[kSlots];
	uint32_t waitedJobless = 0;
	for (auto &job : queue.jobs)
		if (job.active && job.waiters && job.producer) {
			++job.refs;
			jobless[waitedJobless++] = &job;
		}
	if (!late && !waitedJobless) {
		IOLockUnlock(queue.lock);
		return;
	}
	if (late)
		++late->refs;
	tgm_chan *channel = queue.space.channel;
	const uint32_t sequence = late ? late->sequence : 0;
	IOLockUnlock(queue.lock);
	uint64_t next = late ? tgm_chan_check_progress(channel, "host1x bridge", sequence) : 0;
	for (uint32_t i = 0; i < waitedJobless; ++i) {
		const uint64_t step = switch_gpu_producer_check(jobless[i]->covering);
		if (step && (!next || step < next))
			next = step;
	}
	IOLockLock(queue.lock);
	if (next && !queue.checkAt && anyWaited())
		armHangCheck(next);
	if (late)
		releaseLocked(*late);
	for (uint32_t i = 0; i < waitedJobless; ++i)
		releaseLocked(*jobless[i]);
	IOLockUnlock(queue.lock);
}

void retire(thread_call_param_t, thread_call_param_t)
{
	struct Completion {
		switch_gpu_bridge *job;
		void (*fn)(void *, int);
		void *context;
		int result;
	};

	Completion callbacks[kSlots];
	uint32_t count = 0;
	IOLockLock(queue.lock);
	__atomic_store_n(&queue.waitingSources, 1u, __ATOMIC_RELEASE);
	bool pendingSources = false;
	int failed = 0;
	const uint32_t completed =
		queue.space.channel ? tgm_chan_settled(queue.space.channel, &failed) : 0;
	for (auto &job : queue.jobs) {
		if (!job.active)
			continue;
		if (job.producer && !settleJobless(job)) {
			pendingSources = true;
			continue;
		}
		const bool channelFailed = failed && !job.producer;
		if (!job.producer) {
			if (!failed && int32_t(completed - job.sequence) < 0)
				continue;
			const int source = producers(job);
			if (!failed && !source) {
				pendingSources = true;
				continue;
			}
			job.result = failed ? -5 : source == 1 ? 1 : source < 0 ? source : -5;
		}
		if (channelFailed) {
			const int released = tegra_host1x_syncpt_release_failed(
				job.syncpoint, job.threshold, TGM_SYNCPT_INCRS);
			// Whatever waits on the old syncpoint fails or times out on its own engine; this slot no longer shares it.
			if (released) {
				IOLog("SwitchGpuBridge: syncpoint %u of a failed job stays short "
				      "of %u "
				      "(%d); its slot takes "
				      "another\n",
				      job.syncpoint, job.threshold, released);
				job.syncpoint = 0;
			}
		}
		if (job.result == 1)
			for (uint32_t i = 0; i < job.count; ++i)
				if (job.tokens[i].engine == SWITCH_GPU_ENGINE_GM20B)
					(void)switch_gpu_timeline_promote(&job.tokens[i]);
		job.active = false;
		if (job.notified) {
			job.notified = false;
			--job.waiters;
		}
		IOLockWakeup(queue.lock, &job, false);
		releaseLocked(job);
	}
	for (auto &job : queue.jobs) {
		if (!job.active && job.completed) {
			callbacks[count++] = {&job, job.completed, job.context, job.result};
			job.completed = nullptr;
		}
	}
	if (queue.checkAt && !anyWaited()) {
		thread_call_cancel(queue.hangCheck);
		queue.checkAt = 0;
	}
	__atomic_store_n(&queue.waitingSources, pendingSources, __ATOMIC_RELEASE);
	IOLockUnlock(queue.lock);
	for (uint32_t i = 0; i < count; ++i) {
		const auto &callback = callbacks[i];
		callback.fn(callback.context, callback.result == 1 ? 0 : callback.result);
		IOLockLock(queue.lock);
		releaseLocked(*callback.job);
		IOLockUnlock(queue.lock);
	}
}
}  // namespace switch_gpu_bridge_queue
