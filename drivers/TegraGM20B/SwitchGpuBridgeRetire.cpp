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

// A jobless slot by its covering job's channel: done once that job drained or the channel stopped short of it. A token's
// own fence inside that job releases with no L2 flush (gpu_exec.c chan_job_fence): the tokens' writes reach DRAM only
// at the covering job's drain, and its increments follow that drain. So a job that died before its fence put them in
// DRAM exactly when the syncpoint reached the threshold, and only short of it are they lost and the increments owed
// (`owed`). The tokens are read before the channel, so a kill that failed one of them is already in `stopped`.
bool settleJobless(switch_gpu_bridge &job, bool &owed)
{
	const int source = producers(job);
	int stopped = 0;
	const int cover = switch_gpu_producer_covered(job.covering, &stopped);
	if (cover == 0) {
		// The producer still runs: a token that failed elsewhere fails the slot now, which stays for that producer (a
		// release if it stops short, the hang check's look at it) until the covering job resolves
		if (source < 0)
			__atomic_store_n(&job.result, source, __ATOMIC_RELEASE);
		return false;
	}
	if (cover == 1 && source == 0)
		return false;
	const bool drained = cover == 1 ||
			     (source == 1 &&
			      int32_t(tegra_host1x_syncpt_read(job.producer) - job.threshold) >= 0);
	__atomic_store_n(&job.result, source < 0 ? source : drained ? 1 : -5, __ATOMIC_RELEASE);
	owed = stopped && !drained;
	return true;
}

// A retired slot: its tokens promoted when it succeeded, its waiters' marks dropped, its sleepers woken
void finish(switch_gpu_bridge &job)
{
	if (job.result == 1)
		for (uint32_t i = 0; i < job.count; ++i)
			if (job.tokens[i].engine == SWITCH_GPU_ENGINE_GM20B)
				(void)switch_gpu_timeline_promote(&job.tokens[i]);
	job.active = false;
	if (job.notified) {
		job.notified = false;
		--job.waiters;
	}
	if (job.watched) {
		job.watched = false;
		--job.waiters;
	}
	IOLockWakeup(queue.lock, &job, false);
	releaseLocked(job);
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
// channel itself. One timer does it, armed when a wait, a notify or a watch begins, cancelled when no waited bridge is
// left (retire), and re-armed for the soonest next step of tgm_chan_check_progress's rule. The references keep the slots
// across the unlocked checks.
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

	struct Owed {
		uint32_t syncpoint, threshold;
	};

	Completion callbacks[kSlots];
	uint32_t count = 0;
	Owed owed[kSlots];
	uint32_t owing = 0;
	IOLockLock(queue.lock);
	__atomic_store_n(&queue.waitingSources, 1u, __ATOMIC_RELEASE);
	bool pendingSources = false;
	int failed = 0;
	const uint32_t completed =
		queue.space.channel ? tgm_chan_settled(queue.space.channel, &failed) : 0;
	for (auto &job : queue.jobs) {
		if (!job.active)
			continue;
		bool owes = false;
		if (job.producer && !settleJobless(job, owes)) {
			pendingSources = true;
			continue;
		}
		if (owes)
			owed[owing++] = {job.producer, job.threshold};
		const bool channelFailed = failed && !job.producer;
		if (!job.producer) {
			if (!failed && int32_t(completed - job.sequence) < 0)
				continue;
			const int source = producers(job);
			if (!failed && !source) {
				pendingSources = true;
				continue;
			}
			__atomic_store_n(&job.result,
					 failed        ? -5
					 : source == 1 ? 1
					 : source < 0  ? source
						       : -5,
					 __ATOMIC_RELEASE);
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
		finish(job);
	}
	// A kill can land between two slots' reads in one pass: a slot on an owed syncpoint that read its producer running
	// reads it again before a release passes its threshold, and sees the stop the owing slot saw
	for (uint32_t i = 0; i < owing; ++i)
		for (auto &job : queue.jobs) {
			bool owes = false;
			if (!job.active || job.producer != owed[i].syncpoint ||
			    !settleJobless(job, owes))
				continue;
			if (owes)
				owed[owing++] = {job.producer, job.threshold};
			finish(job);
		}
	// After every slot of the pass stored its failure: a release cancels each stream waiting on that producer syncpoint,
	// whichever slot it came through, and a stream queued after it finds its own slot's failure (switch_gpu_bridge_failed)
	for (uint32_t i = 0; i < owing; ++i)
		switch_gpu_producer_release(owed[i].syncpoint, owed[i].threshold);
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
