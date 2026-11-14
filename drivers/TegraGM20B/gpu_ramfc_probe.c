// SPDX-License-Identifier: MIT
// What the host keeps of each channel's RAMFC timeslice, for debug.switch_gpu_ramfc (TegraGM20BRamfcProbe.cpp) and HW
// Test gpu.ramfc_timeslice: debug kernels only.
#include "gpu_internal.h"

#if TGM_STATS
/* Under tgm_gpu_held (the GPU up: nvkm_kmap flushes the L2) and tgm_live_lock (each channel's instance block kept). Only
 * channels with a job completed since the write count: that job's load and save of RAMFC are what a write that raced
 * the host's save loses to. */
static void ramfc_look(struct nvkm_device *device, void *arg)
{
	struct tgm_ramfc_look *look = arg;
	struct tgm_chan *chan = NULL;

	(void)device;
	mutex_lock(&tgm_live_lock);
	list_for_each_entry(chan, &tgm_live_chans, live)
	{
		u32 value = 0;

		look->channels++;
		if (chan->kernel || READ_ONCE(chan->error) || READ_ONCE(chan->timeslice_due) ||
		    (s32)(tgm_chan_completed(chan) - chan->timeslice_at) <= 0)
			continue;
		value = tgm_ramfc_read(chan->chid, RAMFC_TIMESLICE);
		look->checked++;
		look->high += chan->timeslice == RAMFC_TIMESLICE_HIGH;
		if (value != chan->timeslice && !look->mismatched++) {
			look->chid = chan->chid;
			look->ramfc = value;
			look->expected = chan->timeslice;
		}
	}
	mutex_unlock(&tgm_live_lock);
}

int tgm_ramfc_look(struct tgm_ramfc_look *look)
{
	*look = (struct tgm_ramfc_look){0};
	return tgm_gpu_held(ramfc_look, look);
}
#endif
