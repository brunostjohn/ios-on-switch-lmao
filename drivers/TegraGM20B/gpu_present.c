// SPDX-License-Identifier: MIT
// The GPU present: the display's frames copied into its scanout on a kernel channel.
#include "gpu_internal.h"

#define CHAN_PRESENT 1 /* the GPU present's channel */
struct present_state present = {.lock = {NULL}};

static void present_typed_complete(void *unused)
{
	void (*done)(void *, int);
	void *arg;
	int status;
	(void)unused;
	mutex_lock(&present.lock);
	done = present.done;
	arg = present.arg;
	status = present.completion_status;
	mutex_unlock(&present.lock);
	status = switch_gpu_present_tokens_finish_visible(&present.tokens, status, switch_gpu_timeline_promote);
	mutex_lock(&present.lock);
	__atomic_store_n(&present.typed_pending, 0, __ATOMIC_RELEASE);
	mutex_unlock(&present.lock);
	done(arg, status);
	__atomic_fetch_sub(&present.completing, 1, __ATOMIC_SEQ_CST);
}

int tgm_present_outstanding(const void *arg)
{
	if (__atomic_load_n(&present.pending, __ATOMIC_SEQ_CST) && present.arg == arg)
		return 1;
	return __atomic_load_n(&present.completing, __ATOMIC_SEQ_CST) != 0;
}

/* live_lock held; these descriptions only run after the present deadline expires. */
static void present_channel_brief(struct tgm_chan *chan, int chid, char *buf, unsigned int len)
{
	if (!chan) {
		scnprintf(buf, len, "untracked ch %d", chid);
		return;
	}
	scnprintf(buf, len, "%.16s ch %u fence %u/%u err %u uid %llu", chan->owner, chan->chid,
		  tgm_chan_completed(chan), chan->submitted, chan->error, chan->id);
}

/* TGM_PRESENT_NS after a present was queued, if it has not completed: name its pending producer. */
static void present_deadline(void *arg)
{
	struct tgm_chan *chan = NULL;
	struct tgm_chan *waited = NULL;
	struct switch_gpu_present_pending pending;
	char state[TGM_HANG_LINE];
	char what[96], current_owner[96], next_owner[96];
	u64 age = 0;
	u32 i = 0;
	u32 seqno = 0;
	u32 eng;

	(void)arg;
	mutex_lock(&present.lock);
	if (!__atomic_load_n(&present.pending, __ATOMIC_ACQUIRE)) {
		mutex_unlock(&present.lock);
		return;
	}
	/* a deadline of an earlier present that was already running when that one completed: this one has its own time,
	 * which a recovery under way restarts */
	age = nvxnu_nanotime() - max(present.queued, present.recovered);
	if (age < TGM_PRESENT_NS) {
		nvxnu_work_schedule_after(present.deadline_work, TGM_PRESENT_NS - age);
		mutex_unlock(&present.lock);
		return;
	}
	age = nvxnu_nanotime() - present.queued;
	if ((s32)(tgm_chan_completed(present.chan) - present.pending) >= 0) {
		nvxnu_panic("tegra-gm20b: present fence %u complete after %llu ms but its nonstall "
			    "interrupt never came",
			    present.pending, age / 1000000);
	}
	/* Timeline status may lock engine state; never query it while holding live_lock. */
	pending = switch_gpu_present_tokens_pending(&present.tokens);
	mutex_lock(&live_lock);
	if (pending.token.engine == SWITCH_GPU_ENGINE_GM20B) {
		chan = live_by_uid(pending.token.queue);
		if (chan && switch_gpu_present_pending_matches(&pending, chan->id, tgm_chan_completed(chan))) {
			waited = chan;
			seqno = (u32)pending.token.value;
		}
	}
	for (i = 0; i < present.waits && !waited; i++) {
		list_for_each_entry(chan, &live_chans, live)
		{
			if (chan->id == present.wait[i].id &&
			    (s32)(tgm_chan_completed(chan) - present.wait[i].seqno) < 0) {
				waited = chan;
				seqno = present.wait[i].seqno;
				break;
			}
		}
	}
	if (hang_recover(waited ? waited : present.chan, "present",
			 waited ? seqno : present.pending, age) != TGM_HANG_STUCK) {
		mutex_unlock(&live_lock);
		present.recovered = nvxnu_nanotime();
		nvxnu_work_schedule_after(present.deadline_work, TGM_PRESENT_NS);
		mutex_unlock(&present.lock);
		return;
	}
	if (waited) {
		present_channel_brief(waited, waited->chid, what, sizeof(what));
	} else {
		scnprintf(what, sizeof(what), "no pending GPU producer; legacy waits %u", present.waits);
	}
	eng = nvkm_rd32(gpu.device, PFIFO_ENG_STATUS(0));
	present_channel_brief(eng & PFIFO_ENG_ID_TSG ? NULL : live_by(0, PFIFO_ENG_ID(eng)),
			      PFIFO_ENG_ID(eng), current_owner, sizeof(current_owner));
	present_channel_brief(eng & PFIFO_ENG_NEXT_ID_TSG ? NULL : live_by(0, PFIFO_ENG_NEXT_ID(eng)),
			      PFIFO_ENG_NEXT_ID(eng), next_owner, sizeof(next_owner));
	chan_describe(present.chan, state, sizeof(state));
	nvxnu_panic("tegra-gm20b: present timeout %llums; typed %u pending %u:%u:%llu status %d physical %d; "
		    "GR %08x current{%.80s} next{%.80s}; waiting{%.80s} seq %u; present{%.448s}",
		    (nvxnu_nanotime() - present.queued) / 1000000, present.tokens.count,
		    pending.token.engine, pending.token.queue, pending.token.value, pending.status, pending.physical,
		    eng, current_owner, next_owner, what, seqno, state);
}

#if TGM_STATS
void present_stats(u32 *presents, u64 *sum, u64 *max)
{
	*presents = __atomic_exchange_n(&present.presents, 0, __ATOMIC_RELAXED);
	*sum = __atomic_exchange_n(&present.lat_sum, 0, __ATOMIC_RELAXED);
	*max = __atomic_exchange_n(&present.lat_max, 0, __ATOMIC_RELAXED);
}

#endif
int tgm_present_open(struct tgm_vm **pvm)
{
	volatile u32 *probe = NULL;
	u64 probe_pa = 0;
	int ret = 0;

	if (gpu.stage < 3)
		return -ENODEV;
	mutex_lock(&present.lock);
	if (present.chan)
		goto out;
	/* the fence pages exist from the first connection on; make sure they do now */
	ret = tgm_fence_page_get(&probe, &probe_pa);
	if (ret)
		goto out;
	tgm_fence_page_put(probe);
	present.deadline_work = nvxnu_work_alloc(present_deadline, NULL);
	present.completion_work = nvxnu_work_alloc(present_typed_complete, NULL);
	ret = present.deadline_work && present.completion_work ? tgm_vm_new(&present.vm) : -ENOMEM;
	if (!ret) {
		ret = pfence_get(
			present.vm); /* the channel's own fence: the page every address space may wait on */
	}
	if (!ret) {
		present.fence = pfence.va;
		present.fence_pa = pfence.pa;
		present.fence_mem = pfence.mem;
		ret = tgm_vm_kmap(present.vm, present.fence_mem, PAGE_SIZE, &present.fence_gpu);
	}
	if (!ret)
		ret = tgm_mem_new(present.vm, fences_pas, TGM_FENCE_GPU_PAGES, &present.fences_mem);
	if (!ret) {
		ret = tgm_vm_kmap(present.vm, present.fences_mem, TGM_FENCE_BYTES,
				  &present.fences_gpu);
	}
	if (!ret) {
		ret = tgm_chan_new_copy(present.vm, NULL, present.fence, present.fence_gpu, "present", 1,
				   &present.chan);
	}
	if (!ret) {
		present.chan->kernel = CHAN_PRESENT;
		gpu_lock();
		chan_prioritize(present.chan);
		mutex_unlock(&gpu.lock);
	} else {
		/* stays unusable: every offer is refused and the display copies with the CPU */
		nvxnu_log("tegra-gm20b: GPU present unavailable: %d\n", ret);
	}
out:
	*pvm = present.chan ? present.vm : NULL;
	mutex_unlock(&present.lock);
	if (present.chan)
		return 0;
	return ret ? ret : -ENODEV;
}

/* Present mappings: volatile, at a fixed address of the present's user range. */
int tgm_present_map(uint64_t va, struct tgm_mem *mem, uint64_t bytes, uint8_t kind)
{
	return present.vm ? tgm_vm_map_raw(present.vm, va, bytes, mem, 0, kind, 0, 1, 0, NULL)
			  : -ENODEV;
}

int tgm_present_unmap(uint64_t va, uint64_t bytes)
{
	return present.vm ? tgm_vm_unmap(present.vm, va, bytes) : -ENODEV;
}

/* A present waits only for the address spaces that import its surface; a surface no address space imports by its first
 * page is the all-channel wait. */
static int present_writer_on(void)
{
	return 1;
}

/* Whether user's address space imports the present's source or its other surface (also_pa, 0: none). */
static int present_imports(const struct tgm_chan *user, uint64_t src_pa, uint64_t also_pa)
{
	return vm_imports(user->vm, src_pa) || (also_pa && vm_imports(user->vm, also_pa));
}
#if TGM_STATS
/* Each read-back waits for the copies before it (NON_PIPELINED: the scanout buffer is read after the frame's copy wrote
 * it); the fence after them flushes their writes past the L2. */
static void present_push_reads(struct nvif_push *push, const struct tgm_present_read *reads, u32 n)
{
	u32 i = 0;

	for (i = 0; i < n; i++) {
		const struct tgm_present_read *r = &reads[i];
		const struct tgm_copy_side src = {r->src_va, r->src_pitch, r->src_block_log2};
		const struct tgm_copy_side dst = {r->dst_va, r->dst_pitch, -1};

		push->cur += tgm_copy_texels(push->cur, &src, &dst, r->width, r->height);
	}
}

#endif
static int present_copy(uint64_t src_va, uint64_t src_pa, uint64_t also_pa, uint64_t stride,
			int src_block, uint32_t width, uint32_t height, uint64_t dst_va,
			uint32_t dst_pitch, uint32_t block_height_log2, void (*done)(void *, int),
			void *arg, const struct tgm_present_read *reads, u32 nreads,
			const struct switch_gpu_backing_token *dependencies, u32 dependency_count,
			int exact)
{
	struct tgm_chan *chan = present.chan;
	struct tgm_chan *user = NULL;
	struct nvif_push *push = NULL;
	u32 *push_start = NULL;
	u64 va = 0;
	u32 n = 0;
	u32 room = 0;
	int ret = 0;
	int matched = 0;

	if (!chan || chan->error)
		return -ENODEV;
	idle_wake(); /* the run rate before the copy is kicked */
	/* dst_va 0: no copy, only the acquires and the fence (direct scanout: the display scans the surface itself once the
	 * compositor's rendering of it is done) */
	if (dst_va && (block_height_log2 > 5 || dst_pitch < width * 4))
		return -EINVAL;
	mutex_lock(&present.lock);
	if (__atomic_load_n(&present.pending, __ATOMIC_ACQUIRE) ||
	    __atomic_load_n(&present.typed_pending, __ATOMIC_ACQUIRE)) {
		mutex_unlock(&present.lock);
		return -EBUSY;
	}
	ret = switch_gpu_present_tokens_prepare(&present.tokens, dependencies, dependency_count);
	if (!ret)
		ret = backing_dependencies(chan, present.tokens.tokens, dependency_count, 0);
	if (ret) {
		switch_gpu_present_tokens_drop(&present.tokens);
		mutex_unlock(&present.lock);
		return ret;
	}
	/* push room for an acquire per user channel there is now (one made meanwhile has nothing for this frame) */
	if (!exact) {
		mutex_lock(&live_lock);
		list_for_each_entry(user, &live_chans, live) room +=
			room < TGM_SYNC_MAX && !user->kernel;
		mutex_unlock(&live_lock);
	}
	push = &chan->chan.push;
	ret = chan_push_wait(chan,
			     (room + dependency_count) * 5 + 57 + nreads * TGM_COPY_RECT_DWORDS +
				     CHAN_FENCE_DWORDS); /* acquires, a start stamp, the copy, the
								  read-backs, the fence */
	if (ret == -ETIMEDOUT) {
		tgm_chan_stalled(chan, "present: push buffer still full after 1 s", chan->submitted,
				 NSEC_PER_SEC);
	}
	if (ret) {
		switch_gpu_present_tokens_drop(&present.tokens);
		mutex_unlock(&present.lock);
		return ret;
	}
	push_start = push->cur;
	ret = backing_dependencies(chan, present.tokens.tokens, dependency_count, 1);
	if (ret) {
		push->cur = push_start;
		switch_gpu_present_tokens_drop(&present.tokens);
		mutex_unlock(&present.lock);
		return ret;
	}
	/* what the surface's writers (or, with none found, every user channel) have submitted so far and not completed yet */
	if (!exact) {
		mutex_lock(&live_lock);
		list_for_each_entry(user, &live_chans, live)
		{
			if (present_writer_on() && !user->kernel &&
			    present_imports(user, src_pa, also_pa))
				matched = 1;
		}
		list_for_each_entry(user, &live_chans, live)
		{
			uintptr_t off = (uintptr_t)user->fence_cpu - (uintptr_t)fences.va;
			if (user->kernel || READ_ONCE(user->error) || off >= TGM_FENCE_BYTES ||
			    n == room || (matched && !present_imports(user, src_pa, also_pa)) ||
			    (s32)(tgm_chan_completed(user) - user->submitted) >= 0)
				continue;
			present.wait[n].id = user->id;
			present.wait[n].seqno = READ_ONCE(user->submitted);
			present.wait[n].fence =
				(const volatile struct TegraGM20BAbiFence *)user->fence_cpu;
			tgm_hold_need(gpu.device, user->chid);
			present.wait_end[n] =
				stats_on() ? &chan_stamps(user, present.wait[n].seqno)->end : NULL;
			va = present.fences_gpu + off;
			/* nouveau's NVDEF/NVVAL/PUSH_* macros compute in int and repeat their branches. */
			/* NOLINTBEGIN(bugprone-signed-bitwise,bugprone-branch-clone) */
			PUSH_MTHD(push, NV906F, SEMAPHOREA,
				  NVVAL(NV906F, SEMAPHOREA, OFFSET_UPPER, upper_32_bits(va)),
				  SEMAPHOREB, lower_32_bits(va), SEMAPHOREC, present.wait[n].seqno,
				  SEMAPHORED,
				  NVDEF(NV906F, SEMAPHORED, OPERATION, ACQ_GEQ) |
					  NVDEF(NV906F, SEMAPHORED, ACQUIRE_SWITCH, ENABLED));
			/* NOLINTEND(bugprone-signed-bitwise,bugprone-branch-clone) */
			n++;
		}
		mutex_unlock(&live_lock);
	}
	present.waits = n;
	present.writers = matched;
	if (stats_on()) { /* the copy's start, after the acquires: stats_fences times the copy and the gap before it */
		present.stamped = chan->submitted + 1;
		push_stamp(push,
			   chan_stamps_va(chan, present.stamped) +
				   offsetof(struct tgm_stamps, start),
			   present.stamped);
	}
	if (dst_va) {
		PUSH_NVSQ(push, NV906F, NVB06F_WFI, 0);
		PUSH_NVSQ(push, NV906F, NVB06F_FB_FLUSH, 0);
		PUSH_NVSQ(push, NV906F, NVB06F_MEM_OP_C, 0, NVB06F_MEM_OP_D,
			  NVB06F_MEM_OP_D_OPERATION_L2_FLUSH_DIRTY);
		PUSH_NVSQ(push, NV906F, NVB06F_FB_FLUSH, 0);
		const int32_t src_block_log2 =
			src_block ? TEGRA_SCANOUT_TILED_BLOCK_HEIGHT_LOG2 : -1;
		const u32 src_pitch = src_block ? width * 4 : (u32)stride;
		const struct tgm_copy_side src = {src_va, src_pitch, src_block_log2};
		const struct tgm_copy_side dst = {dst_va, dst_pitch, (int32_t)block_height_log2};

		push->cur += tgm_copy_texels(push->cur, &src, &dst, width, height);
	}
#if TGM_STATS
	if (nreads)
		present_push_reads(push, reads, nreads);
#endif
	/* set before the kick: the nonstall interrupt can come before chan_fence returns */
	present.done = done;
	present.arg = arg;
	present.queued = nvxnu_nanotime();
	__atomic_store_n(&present.typed_pending, exact != 0, __ATOMIC_RELEASE);
	__atomic_store_n(&present.pending, chan->submitted + 1, __ATOMIC_RELEASE);
	nvxnu_work_schedule_after(
		present.deadline_work,
		TGM_PRESENT_NS); /* before the kick, so the completion cancels it */
	chan_fence(chan);
	if (matched && stats_on())
		__atomic_fetch_add(&stats.writer, 1, __ATOMIC_RELAXED);
	if (!n && stats_on())
		__atomic_fetch_add(&stats.idle, 1, __ATOMIC_RELAXED);
	mutex_unlock(&present.lock);
	return 0;
}

/* Refused while the gate is shut: the display copies that frame on the CPU. */
int tgm_present_copy(uint64_t src_va, uint64_t src_pa, uint64_t also_pa, uint64_t stride,
		     int src_block, uint32_t width, uint32_t height, uint64_t dst_va,
		     uint32_t dst_pitch, uint32_t block_height_log2, void (*done)(void *, int),
		     void *arg)
{
	int ret = 0;

	if (!gate_try())
		return -EBUSY;
	ret = present_copy(src_va, src_pa, also_pa, stride, src_block, width, height, dst_va,
			   dst_pitch, block_height_log2, done, arg, NULL, 0, NULL, 0, 0);
	gate_leave();
	return ret;
}

int tgm_present_copy_dependencies(uint64_t src_va, uint64_t src_pa, uint64_t also_pa,
				  uint64_t stride, int src_block, uint32_t width, uint32_t height,
				  uint64_t dst_va, uint32_t dst_pitch, uint32_t block_height_log2,
				  void (*done)(void *, int), void *arg,
				  const struct switch_gpu_backing_token *dependencies,
				  uint32_t dependency_count)
{
	int ret;
	if (!gate_try())
		return -EBUSY;
	ret = present_copy(src_va, src_pa, also_pa, stride, src_block, width, height, dst_va,
			   dst_pitch, block_height_log2, done, arg, NULL, 0, dependencies,
			   dependency_count, 1);
	gate_leave();
	return ret;
}
#if TGM_STATS
int tgm_present_copy_read(uint64_t src_va, uint64_t src_pa, uint64_t also_pa, uint64_t stride,
			  int src_block, uint32_t width, uint32_t height, uint64_t dst_va,
			  uint32_t dst_pitch, uint32_t block_height_log2, void (*done)(void *, int),
			  void *arg, const struct tgm_present_read *reads, uint32_t nreads)
{
	int ret = 0;

	if (nreads > TGM_PRESENT_READS)
		return -EINVAL;
	if (!gate_try())
		return -EBUSY;
	ret = present_copy(src_va, src_pa, also_pa, stride, src_block, width, height, dst_va,
			   dst_pitch, block_height_log2, done, arg, reads, nreads, NULL, 0, 0);
	gate_leave();
	return ret;
}

int tgm_present_copy_read_dependencies(uint64_t src_va, uint64_t src_pa, uint64_t also_pa,
				       uint64_t stride, int src_block, uint32_t width,
				       uint32_t height, uint64_t dst_va, uint32_t dst_pitch,
				       uint32_t block_height_log2, void (*done)(void *, int),
				       void *arg, const struct tgm_present_read *reads,
				       uint32_t nreads,
				       const struct switch_gpu_backing_token *dependencies,
				       uint32_t dependency_count)
{
	int ret;
	if (nreads > TGM_PRESENT_READS)
		return -EINVAL;
	if (!gate_try())
		return -EBUSY;
	ret = present_copy(src_va, src_pa, also_pa, stride, src_block, width, height, dst_va,
			   dst_pitch, block_height_log2, done, arg, reads, nreads, dependencies,
			   dependency_count, 1);
	gate_leave();
	return ret;
}
#endif
