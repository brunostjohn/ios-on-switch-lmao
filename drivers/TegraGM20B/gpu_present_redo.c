// SPDX-License-Identifier: MIT
// The present's copy, and its redo after a copy engine reset cut it (tgm_ctxsw_decide).
#include "gpu_internal.h"

void tgm_present_push_copy(struct nvif_push *push)
{
	const u32 src_pitch = tgm_present.copy.src_block_log2 >= 0 ? tgm_present.copy.width * 4
								   : (u32)tgm_present.copy.stride;
	const struct tgm_copy_side src = {tgm_present.copy.src_va, src_pitch,
					  tgm_present.copy.src_block_log2};
	const struct tgm_copy_side dst = {tgm_present.copy.dst_va, tgm_present.copy.dst_pitch,
					  (int32_t)tgm_present.copy.block_height_log2};

	PUSH_NVSQ(push, NV906F, NVB06F_WFI, 0);
	PUSH_NVSQ(push, NV906F, NVB06F_FB_FLUSH, 0);
	PUSH_NVSQ(push, NV906F, NVB06F_MEM_OP_C, 0, NVB06F_MEM_OP_D,
		  NVB06F_MEM_OP_D_OPERATION_L2_FLUSH_DIRTY);
	PUSH_NVSQ(push, NV906F, NVB06F_FB_FLUSH, 0);
	if (stats_on())
		__atomic_fetch_add(&tgm_stats.present_flushes, 1, __ATOMIC_RELAXED);
	push->cur += tgm_copy_texels(push->cur, &src, &dst, tgm_present.copy.width,
				     tgm_present.copy.height);
}

/* The present whose copy a copy engine reset cut (tgm_present_redo): its fence completed, so it is copied again, with no
 * acquire (what it waited for completed before), the L2 flush and the fence as present_copy pushes them. The display
 * keeps scanning the frame before it until this copy's fence completes and present_fenced delivers its completion. */
void tgm_present_redo_work(void *unused)
{
	struct tgm_chan *chan = tgm_present.chan;
	struct nvif_push *push = NULL;
	int ret = 0;

	(void)unused;
	mutex_lock(&tgm_present.lock);
	push = &chan->chan.push;
	ret = chan->error ? -ENODEV
			  : chan_push_wait(chan, TGM_PRESENT_COPY_DWORDS + CHAN_FENCE_DWORDS);
	if (ret) {
		mutex_unlock(&tgm_present.lock);
		nvxnu_panic("tegra-gm20b: the present's copy cut by a copy engine reset cannot be "
			    "redone (%d)",
			    ret);
	}
	tgm_present_push_copy(push);
	tgm_present.queued = nvxnu_nanotime();
	__atomic_store_n(&tgm_present.pending, chan->submitted + 1, __ATOMIC_RELEASE);
	nvxnu_work_schedule_after(tgm_present.deadline_work, TGM_PRESENT_NS);
	chan_fence_volatile(chan);
	mutex_unlock(&tgm_present.lock);
	/* present_fenced kept the completion claimed (tgm_present_outstanding) until the redo is in flight */
	__atomic_fetch_sub(&tgm_present.completing, 1, __ATOMIC_SEQ_CST);
}

/* present_fenced takes pending to 0 before it reads redo: a present that completed before redo was set is seen gone here
 * and the mark withdrawn, so no later present inherits it. */
void tgm_present_redo(void)
{
	u32 pending = __atomic_load_n(&tgm_present.pending, __ATOMIC_SEQ_CST);

	if (!pending || !tgm_present.copy.valid)
		return;
	__atomic_store_n(&tgm_present.redo, pending, __ATOMIC_SEQ_CST);
	if (__atomic_load_n(&tgm_present.pending, __ATOMIC_SEQ_CST) != pending)
		__atomic_compare_exchange_n(&tgm_present.redo, &pending, 0, false, __ATOMIC_SEQ_CST,
					    __ATOMIC_RELAXED);
}
