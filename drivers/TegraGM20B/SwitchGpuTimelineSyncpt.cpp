// SPDX-License-Identifier: MIT
// A GM20B channel's own host1x syncpoint (host1x.md A2). Once a host1x consumer has asked for one, every job whose end
// drains the channel increments it after the job's fence release, so the consumer's stream waits on the producer itself
// and the bridge makes no GPU job of its own. Only draining jobs increment: a job before one is covered by it (the
// channel runs in order, and L2_FLUSH_DIRTY writes every dirty line back), so a token's threshold is that of the first
// draining job at or after it.
#include <IOKit/IOLib.h>
#include "SwitchGpuTimelineInternal.h"
#include "TegraGM20BSyncpt.h"

using namespace switch_gpu_timelines;

namespace
{
// The threshold of the oldest kept draining job at or after `value`. A newer one also covers the token, later.
bool covering_mark(const switch_gpu_timeline_syncpt &syncpt, uint32_t value, uint32_t *threshold)
{
	bool found = false;
	uint32_t best = 0;
	for (const uint64_t &slot : syncpt.marks) {
		const uint64_t mark = __atomic_load_n(&slot, __ATOMIC_ACQUIRE);
		const uint32_t seqno = uint32_t(mark >> 32);
		if (!mark || int32_t(seqno - value) < 0 || (found && int32_t(seqno - best) >= 0))
			continue;
		found = true;
		best = seqno;
		*threshold = uint32_t(mark);
	}
	return found;
}
}  // namespace

extern "C" uint32_t switch_gpu_timeline_syncpt_next(switch_gpu_timeline *timeline, uint32_t seqno)
{
	auto &syncpt = timeline->syncpt;
	const uint32_t id = __atomic_load_n(&syncpt.id, __ATOMIC_ACQUIRE);
	if (!id)
		return 0;
	syncpt.increments += TGM_SYNCPT_INCRS;
	__atomic_store_n(&syncpt.marks[syncpt.increments / TGM_SYNCPT_INCRS % kSyncptMarks],
			 uint64_t(seqno) << 32 | uint32_t(syncpt.base + syncpt.increments),
			 __ATOMIC_RELEASE);
	return id;
}

extern "C" int switch_gpu_timeline_syncpoint(const switch_gpu_backing_token *token, uint32_t *id,
					     uint32_t *threshold)
{
	if (!lock)
		return -19;
	IOLockLock(lock);
	const auto *timeline = find(token);
	const uint32_t value = uint32_t(token->value);
	int result = !timeline ? -19 : !timeline->syncpt.id ? -61 : 1;
	if (result == 1 && (__atomic_load_n(&timeline->terminal, __ATOMIC_ACQUIRE) ||
			    failed_locked(timeline, value)))
		result = 0;
	for (auto *node = result == 1 ? timeline->semantics : nullptr; node; node = node->next)
		if (int32_t(value - node->seqno) >= 0)
			result = 0;
	if (result == 1 && !covering_mark(timeline->syncpt, value, threshold))
		result = 0;
	if (result == 1)
		*id = timeline->syncpt.id;
	IOLockUnlock(lock);
	return result;
}

extern "C" int switch_gpu_timeline_syncpt_adopt(const switch_gpu_backing_token *token, uint32_t id,
						uint32_t base)
{
	IOLockLock(lock);
	auto *timeline = find(token);
	const int result = !timeline ? -19 : timeline->syncpt.id ? -16 : 0;
	if (!result) {
		timeline->syncpt.base = base;
		__atomic_store_n(&timeline->syncpt.id, id, __ATOMIC_RELEASE);
	}
	IOLockUnlock(lock);
	return result;
}

extern "C" uint32_t switch_gpu_timeline_syncpt_reclaim(uint32_t *ids, uint32_t max)
{
	IOLockLock(lock);
	uint32_t taken = 0;
	while (taken < max && syncpt_returned_count)
		ids[taken++] = syncpt_returned[--syncpt_returned_count];
	IOLockUnlock(lock);
	return taken;
}
