// SPDX-License-Identifier: MIT
// Debug kernels: a channel's GR context state as GR holds it, once per channel.
#include "gpu_internal.h"

#if TGM_STATS
/* FECS_CURRENT_CTX: VALID bit 31, the resident context's instance block in 27:0 (4 KiB units), as gf100_gr_intr reads it */
#define FECS_CURRENT_CTX 0x409b00
#define FECS_CURRENT_CTX_VALID 0x80000000u
#define PD_NUM_TPC_PER_GPC(i) (0x406028 + (i) * 4)
#define DS_NUM_TPC_PER_GPC 0x405870

static u64 chan_inst_addr(u32 chid)
{
	struct nvkm_runl *runl = NULL;
	struct nvkm_chan *nchan = NULL;
	unsigned long flags = 0;
	u64 addr = 0;

	nvkm_runl_foreach(runl, tgm_gpu.device->fifo)
	{
		nchan = nvkm_runl_chan_get_chid(runl, chid, &flags);
		if (nchan) {
			addr = nchan->inst->addr;
			nvkm_chan_put(&nchan, flags);
			break;
		}
	}
	return addr;
}

/* At chan_new, under tgm_gpu.lock: the fence interrupt runs under nvkm's event lock, where the runlist's locks are not
 * taken. */
void tgm_stats_gr_bind(struct tgm_chan *chan)
{
	chan->gr_inst = chan_inst_addr(chan->chid);
}

/* PD's and DS's TPC counts per GPC are context state: their registers read the resident context's. Taken at one of the
 * channel's fence interrupts while GR still holds its context, with FECS_CURRENT_CTX the same before and after. */
void tgm_stats_gr_context(struct tgm_chan *chan)
{
	struct nvkm_device *device = tgm_gpu.device;
	u32 pd[4];
	u32 ds = 0;
	u32 ctx = 0;
	unsigned int i = 0;

	if (chan->gr_context_logged || !chan->gr_inst)
		return;
	ctx = nvkm_rd32(device, FECS_CURRENT_CTX);
	if (!(ctx & FECS_CURRENT_CTX_VALID) || ((u64)(ctx & 0x0fffffff) << 12) != chan->gr_inst)
		return;
	for (i = 0; i < ARRAY_SIZE(pd); i++)
		pd[i] = nvkm_rd32(device, PD_NUM_TPC_PER_GPC(i));
	ds = nvkm_rd32(device, DS_NUM_TPC_PER_GPC);
	if (nvkm_rd32(device, FECS_CURRENT_CTX) != ctx)
		return;
	chan->gr_context_logged = 1;
	nvxnu_stats("tegra-gm20b: GR context of %s ch %u: PD num_tpc_per_gpc %08x %08x %08x %08x, "
		    "DS %08x; GPU rail-ups so far %u",
		    chan->owner, chan->chid, pd[0], pd[1], pd[2], pd[3], ds,
		    tgm_rail_ups.display + tgm_rail_ups.demand);
}
#endif
