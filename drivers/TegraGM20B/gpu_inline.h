// SPDX-License-Identifier: MIT
// The small helpers of gpu_internal.h that the gpu_*.c units inline, as gpu.c did when they were its statics.
#pragma once
#include "gpu_internal.h"

static inline int badf(u32 v)
{
	return (v & 0xffff0000) == 0xbadf0000;
}

static inline void gate_wake_closer(void)
{
	mutex_lock(&gate_lock);
	nvxnu_wakeup(&sc7_gate.inside);
	mutex_unlock(&gate_lock);
}

/* In (1), or refused while the gate is shut (0). */
static inline int gate_try(void)
{
	enum tgm_gate_entry entry = tgm_gate_try_enter(&sc7_gate);

	if (entry == TGM_GATE_SHUT_WAKE)
		gate_wake_closer();
	return entry == TGM_GATE_IN;
}

static inline void gate_leave(void)
{
	if (tgm_gate_leave(&sc7_gate))
		gate_wake_closer();
}

/* Cacheable user mappings share GPU L2. Legacy submissions acquire CPU writes and publish DRAM at every fence.
 * Explicit jobs acquire only new CPU/external-engine memory epochs and page-table changes; internal completion
 * preserves execution order without forcing DRAM writeback. CPU, host1x and display consumers require external
 * visibility, published by an external fence or the retained-token bridge. Unmap flushes completed/failed writes
 * before any physical page can be reused. Volatile command/control mappings bypass L2 but still require the CPU's
 * pre-kick DSB and the shader/constant cache transitions in the command stream. */
static inline int l2_on(void)
{
	return 1;
}

static inline void mem_big_views_drop(struct tgm_mem *mem)
{
	while (mem->big_views) {
		struct switch_gpu_memory_view *view = mem->big_views;
		mem->big_views = view->next;
		nvif_mem_dtor(&view->mem);
		kfree(view);
	}
}

static inline int vm_imports(struct tgm_vm *vm, u64 pa) /* live_lock held */
{
	struct tgm_import *im = NULL;
	if (vm->imports_lost)
		return 1;
	list_for_each_entry(im, &vm->imports, head)
	{
		if (im->pa == pa)
			return 1;
	}
	return 0;
}

/* 1 if the address space has a bind its channels' next submission must wait for (and *target, *va set) */
static inline int bind_pending(const struct tgm_vm *vm, u32 *target, u64 *va)
{
	const volatile u32 *cpu = __atomic_load_n(&vm->bind_cpu, __ATOMIC_ACQUIRE);

	if (!cpu)
		return 0;
	*target = __atomic_load_n(&vm->bind_target, __ATOMIC_ACQUIRE);
	*va = READ_ONCE(vm->bind_va);
	return (s32)(__atomic_load_n(cpu, __ATOMIC_ACQUIRE) - *target) < 0;
}

static inline void chan_wake_all_waiters(struct tgm_chan *chan)
{
	u32 waiter = tgm_chan_waiter(chan);

	if (stats_on()) {
		__atomic_store_n(&waiter_woken_ns[waiter], nvxnu_nanotime(), __ATOMIC_RELAXED);
		__atomic_fetch_add(&wstat.wakes, 1, __ATOMIC_RELAXED);
	}
	nvxnu_word_wake(&waiter_words[waiter]);
}

/* live_lock held; by id, else (id 0) by hardware channel */
static inline struct tgm_chan *live_by(u64 id, int chid)
{
	struct tgm_chan *chan = NULL;

	list_for_each_entry(chan, &live_chans, live)
	{
		if (id ? chan->id == id : (int)chan->chid == chid)
			return chan;
	}
	return NULL;
}

static inline int stats_on(void)
{
	return TGM_STATS;
}

/* The atomics below write through sum and max. */
/* NOLINTNEXTLINE(readability-non-const-parameter) */
static inline void stats_add(u64 *sum, u64 *max, u64 v)
{
	u64 m = __atomic_load_n(max, __ATOMIC_RELAXED);
	__atomic_fetch_add(sum, v, __ATOMIC_RELAXED);
	while (v > m &&
	       !__atomic_compare_exchange_n(max, &m, v, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
		;
}

static inline volatile struct tgm_stamps *chan_stamps(struct tgm_chan *chan, u32 seqno)
{
	return (volatile struct tgm_stamps *)((u8 *)chan->ring + CHAN_STAMPS) +
	       seqno % CHAN_NSTAMPS;
}

static inline u64 chan_stamps_va(struct tgm_chan *chan, u32 seqno)
{
	return chan->ring_va + CHAN_STAMPS +
	       (u64)(seqno % CHAN_NSTAMPS) * sizeof(struct tgm_stamps);
}

static inline void stats_fences(struct tgm_chan *chan)
{
	u32 done = tgm_chan_completed(chan);
	u32 s = chan->stat_done;
	u32 i = 0;
	u64 now = nvxnu_nanotime();
	u64 start = 0;
	u64 end = 0;
	u64 gap = 0;
	volatile struct tgm_stamps *st = NULL;

	if ((s32)(done - s) > CHAN_NSTAMPS) {
		s = done - CHAN_NSTAMPS;
		chan->stat_run.live = 0;
	}
	for (; (s32)(done - s) > 0; s++) {
		i = (s + 1) % CHAN_NSTAMPS;
		if (!chan->kernel) {
			__atomic_fetch_add(&stats.fences, 1, __ATOMIC_RELAXED);
			stats_add(&stats.lat_sum, &stats.lat_max,
				  now - READ_ONCE(chan->stat_sub_ns[i]));
			stats_job_seqno(chan, s + 1);
		}
		st = chan_stamps(chan, s + 1);
		if (!chan->kernel && s + 1 == READ_ONCE(chan->prof_seqno)) {
			u32 first = READ_ONCE(chan->prof_first);
			volatile struct tgm_stamps *fs = chan_stamps(chan, first);

			if (fs->start.payload == first && st->end.payload == s + 1)
				prof_collect(chan, first, fs->start.ns, st->end.ns);
			WRITE_ONCE(chan->prof_seqno, 0);
		}
		if (st->start.payload != s + 1 || st->end.payload != s + 1)
			continue; /* overwritten by a later submission, or not stamped */
		start = st->start.ns;
		end = st->end.ns;
		if (chan->kernel) {
			__atomic_fetch_add(&stats.copies, 1, __ATOMIC_RELAXED);
			stats_add(&stats.copy_sum, &stats.copy_max, end - start);
			gap = present_gap(s + 1, start);
			if (gap != ~0ULL) {
				__atomic_fetch_add(&stats.gaps, 1, __ATOMIC_RELAXED);
				stats_add(&stats.gap_sum, &stats.gap_max, gap);
			}
			continue;
		}
		__atomic_fetch_add(&stats.timed, 1, __ATOMIC_RELAXED);
		stats_add(&stats.queue_sum, &stats.queue_max,
			  start - READ_ONCE(chan->stat_sub_ptimer[i]));
		if (chan->high) {
			__atomic_fetch_add(&stats.comp, 1, __ATOMIC_RELAXED);
			stats_add(&stats.comp_sum, &stats.comp_max,
				  start - READ_ONCE(chan->stat_sub_ptimer[i]));
			if (READ_ONCE(chan->stat_kick[i]) != TGM_KICK_CLEAR) {
				__atomic_fetch_add(&stats.behind, 1, __ATOMIC_RELAXED);
				stats_add(&stats.behind_sum, &stats.behind_max,
					  start - READ_ONCE(chan->stat_sub_ptimer[i]));
			}
		}
	}
	chan->stat_done = done;
}

/* Before a submission's work is kicked (the submitter's thread): busy now, and the run rate first. The order (busy
 * stamp, then the drop flags) pairs with gov_drop's (flag, then the stamp): one of the two always sees the other. */
static inline void idle_wake(void)
{
	if (!READ_ONCE(idle.on))
		return;
	__atomic_store_n(&idle.last_busy, nvxnu_nanotime(), __ATOMIC_SEQ_CST);
	if (__atomic_load_n(&idle.low, __ATOMIC_SEQ_CST) ||
	    __atomic_load_n(&idle.dropping, __ATOMIC_SEQ_CST))
		(void)idle_raise();
}

/* gpu.lock held */
static inline void chan_prioritize(struct tgm_chan *chan)
{
	chan->high = 1;
	tgm_sched_level(gpu.device, chan->chid, TGM_LEVEL_HIGH);
}

/* live_lock held, `chan` leaving: what other channels had queued on its fence word goes (it is idle: nothing to wait for) */
static inline void order_forget(struct tgm_chan *chan)
{
	struct tgm_chan *other = NULL;
	u32 i = 0;

	list_for_each_entry(other, &live_chans, live)
	{
		for (i = 0; i < other->norder;) {
			if (other->order[i].id == chan->id) {
				other->order[i] = other->order[--other->norder];
			} else {
				i++;
			}
		}
	}
}

static inline int sync_done(u64 id, u32 seqno)
{
	struct tgm_chan *chan = NULL;
	int done = 1; /* a channel that is gone was drained or stopped by tgm_chan_del */
	mutex_lock(&live_lock);
	list_for_each_entry(chan, &live_chans, live)
	{
		if (chan->id == id) {
			done = chan->error || (s32)(tgm_chan_completed(chan) - seqno) >= 0;
			break;
		}
	}
	mutex_unlock(&live_lock);
	return done;
}

/* live_lock held */
static inline struct tgm_chan *live_by_uid(u32 uid)
{
	struct tgm_chan *chan = NULL;

	list_for_each_entry(chan, &live_chans, live)
	{
		if ((u32)chan->id == uid)
			return chan;
	}
	return NULL;
}

/* One 16-byte semaphore release (writes payload and the GPU's PTIMER ns), after wait-for-idle */
static inline void push_stamp(struct nvif_push *push, u64 va, u32 payload)
{
	/* nouveau's NVDEF/NVVAL/PUSH_* macros compute in int and repeat their branches. */
	/* NOLINTBEGIN(bugprone-signed-bitwise,bugprone-branch-clone) */
	PUSH_MTHD(push, NV906F, SEMAPHOREA,
		  NVVAL(NV906F, SEMAPHOREA, OFFSET_UPPER, upper_32_bits(va)), SEMAPHOREB,
		  lower_32_bits(va), SEMAPHOREC, payload, SEMAPHORED,
		  NVDEF(NV906F, SEMAPHORED, OPERATION, RELEASE) |
			  NVDEF(NV906F, SEMAPHORED, RELEASE_WFI, EN) |
			  NVDEF(NV906F, SEMAPHORED, RELEASE_SIZE, 16BYTE));
	/* NOLINTEND(bugprone-signed-bitwise,bugprone-branch-clone) */
}

static inline int chan_wait_room(struct tgm_chan *chan, u32 gpfifo_nr, u32 push_nr)
{
	int ret = chan_wait_ready(chan, gpfifo_nr, push_nr);
	return ret ? ret : nvif_chan_gpfifo_wait(&chan->chan, gpfifo_nr, push_nr);
}

/* nvif's PUSH_WAIT for push_nr dwords, waited for as chan_wait_room does. */
static inline int chan_push_wait(struct tgm_chan *chan, u32 push_nr)
{
	struct nvif_push *push = &chan->chan.push;

	return push->cur + push_nr > push->end ? chan_wait_room(chan, 1, push_nr) : 0;
}

static inline u32 chan_fence_visibility(struct tgm_chan *chan, int external)
{
	return chan_fence_job(chan, external, chan->submitted + 1, chan->submitted + 1);
}

static inline u32 chan_fence(struct tgm_chan *chan)
{
	return chan_fence_visibility(chan, 1);
}

/* From the present channel's nonstall interrupt (the interrupt pass, nvkm's event lock held): a present whose fence passed
 * is done, and its callback runs here (the display's only signals its work loop). completing covers the claim to the
 * callback's return, so tgm_present_outstanding never answers 0 for a present whose completion is not delivered yet. */
static inline void present_fenced(struct tgm_chan *chan)
{
	u32 pending = __atomic_load_n(&present.pending, __ATOMIC_ACQUIRE);
	void (*done)(void *, int) = NULL;
	void *arg = NULL;
	u64 queued = 0;
	int status = 0;

	if (!pending || (s32)(tgm_chan_completed(chan) - pending) < 0)
		return;
	done = present.done;
	arg = present.arg;
	queued = present.queued;
	status = present_source_lost() ? TEGRA_SCANOUT_SOURCE_LOST : 0;
	__atomic_fetch_add(&present.completing, 1, __ATOMIC_SEQ_CST);
	if (!__atomic_compare_exchange_n(&present.pending, &pending, 0, false, __ATOMIC_SEQ_CST,
					 __ATOMIC_RELAXED)) {
		__atomic_fetch_sub(&present.completing, 1, __ATOMIC_SEQ_CST);
		return;
	}
	nvxnu_work_cancel(present.deadline_work);
	if (stats_on()) {
		__atomic_fetch_add(&present.presents, 1, __ATOMIC_RELAXED);
		stats_add(&present.lat_sum, &present.lat_max, nvxnu_nanotime() - present.queued);
	}
	tgm_deadline_presented(queued);
	if (__atomic_load_n(&present.typed_pending, __ATOMIC_ACQUIRE)) {
		present.completion_status = status;
		nvxnu_work_schedule(present.completion_work);
		return;
	}
	done(arg, status);
	__atomic_fetch_sub(&present.completing, 1, __ATOMIC_SEQ_CST);
}

/* Pages went back dirty: the work item claims and queues them (any context; nothing before the scrubber is up). */
static inline void scrub_kick(void)
{
	if (READ_ONCE(scrub.chan))
		nvxnu_work_schedule(scrub.work);
}
