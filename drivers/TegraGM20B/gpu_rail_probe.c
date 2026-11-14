// SPDX-License-Identifier: MIT
// What a rail-up left, for debug.switch_gpu_rail_cycle (TegraGM20B.cpp) and HW Test gpu.rail_cycle: debug kernels only.
#include "gpu_internal.h"

#if TGM_STATS
static int kernel_fence(u64 *us)
{
	struct tgm_chan *chan = tgm_scrub.chan ? tgm_scrub.chan : tgm_present.chan;
	struct mutex *lock = tgm_scrub.chan ? &tgm_scrub.lock : &tgm_present.lock;
	u64 start = nvxnu_nanotime();
	u64 end = start + TGM_PRESENT_NS;
	u32 seqno = 0;
	int ret = 0;

	if (!chan || READ_ONCE(chan->error))
		return -ENODEV;
	tgm_gate_enter();
	mutex_lock(lock);
	ret = chan_push_wait(chan, CHAN_FENCE_DWORDS);
	if (!ret)
		seqno = chan_fence(chan);
	mutex_unlock(lock);
	while (!ret) {
		u32 seq = nvxnu_wake_seq();

		if ((s32)(tgm_chan_completed(chan) - seqno) >= 0)
			break;
		if (nvxnu_nanotime() >= end)
			ret = -ETIMEDOUT;
		else
			nvxnu_idle(seq, end);
	}
	gate_leave();
	*us = (nvxnu_nanotime() - start) / 1000;
	return ret;
}

static void gpcpll_look(struct nvkm_device *device, void *arg)
{
	struct tgm_rail_look *look = arg;

	look->coeff = nvkm_rd32(device, GPCPLL_COEFF_R);
	look->cfg = nvkm_rd32(device, GPCPLL_CFG_R);
	look->khz = nvkm_clk_read(device->clk, nv_clk_src_gpc);
	look->pl = (look->coeff >> 16) & 0x3f;
	look->expect_pl = (look->coeff >> 16) & 0x3f;
	look->vco_ok = 1;
	if (tgm_idle.clk) {
		look->expect_pl = READ_ONCE(tgm_idle.low) ? tgm_idle.idle_pl : tgm_idle.run_pl;
		look->vco_ok = (look->coeff & 0xffff) == tgm_idle.vco_coeff;
	}
}

/* The GPU's first job since the ungate, one kernel fence through the host, PBDMA, semaphore release and nonstall
 * interrupt (wake_fence's, with a late fence reported instead of judged hung), then the GPCPLL against the idle clock's
 * rate: enabled and locked, on its VCO, at the PL the idle clock last set. */
int tgm_rail_look(struct tgm_rail_look *look)
{
	u64 us = 0;
	int ret = 0;

	*look = (struct tgm_rail_look){0};
	look->fence = kernel_fence(&us);
	look->fence_us = (u32)us;
	ret = tgm_gpu_held(gpcpll_look, look);
	if (ret)
		return ret;
	return !look->fence && (look->cfg & 0x20001) == 0x20001 && look->vco_ok &&
			       look->pl == look->expect_pl
		       ? 0
		       : -EIO;
}
#endif
