// SPDX-License-Identifier: MIT
/* Debug kernels: the register dump of TegraGM20BRegDump.h. Every register is read once, in list order, into an array
 * with the GPU held still (tgm_gpu_held) and its runlists locked, so no engine reset (recover.c, nvkm's recovery) falls
 * among the reads; the text is made after the locks are let go. */
#include <core/device.h>
#include <engine/fifo.h>
#include <engine/fifo/runl.h>
#include "TegraGM20BGpu.h"

#if TGM_STATS
#include "TegraGM20BRegDump.h"

#define FIFO_ENGINE_STATUS0 0x002640
#define BUS_INTR_0 0x001100
#define BUS_INTR_EN_0 0x001140
#define BUS_INTR_PRI_FAULTS 0x0000000eu /* gf100_bus_intr: !ENGINE, PRIVRING, TIMEOUT */
#define PRI_TIMEOUT_SAVE_0 0x009084
#define PRI_TIMEOUT_SAVE_1 0x009088
#define PMU_MUTEX_ID 0x10a488
#define PMU_MUTEX_ID_RELEASE 0x10a48c
#define PMU_MUTEX_ID_VALUE 0xffu
#define PMU_MUTEX_ID_NOT_AVAIL 0xffu
#define FECS_CURRENT_CTX 0x409b00

struct regdump_pass {
	u32 *values;
	struct tgm_regdump_how *how;
	u32 bus, bus_en, save0, save1;
};

/* Reading PWR_PMU_MUTEX_ID hands out a token from the PMU's mutex id pool (nvgpu gk20a_pmu_mutex_acquire); it goes back
 * as nvgpu gives back one it could not use. The bus's fault registers read as they were before the dump masked and
 * raised them. */
static u32 regdump_read(struct nvkm_device *device, const struct regdump_pass *pass, u32 offset)
{
	u32 value = 0;

	switch (offset) {
	case BUS_INTR_0:
		return pass->bus;
	case BUS_INTR_EN_0:
		return pass->bus_en;
	case PRI_TIMEOUT_SAVE_0:
		return pass->save0;
	case PRI_TIMEOUT_SAVE_1:
		return pass->save1;
	case PMU_MUTEX_ID:
		value = nvkm_rd32(device, PMU_MUTEX_ID);
		if ((value & PMU_MUTEX_ID_VALUE) && (value & PMU_MUTEX_ID_VALUE) != PMU_MUTEX_ID_NOT_AVAIL) {
			nvkm_wr32(device, PMU_MUTEX_ID_RELEASE,
				  (nvkm_rd32(device, PMU_MUTEX_ID_RELEASE) & ~PMU_MUTEX_ID_VALUE) |
					  (value & PMU_MUTEX_ID_VALUE));
		}
		return value;
	default:
		return nvkm_rd32(device, offset);
	}
}

/* After M3 any MMIO fault the bus reports through nvkm's interrupt handler panics (xnu_glue.cpp gpuInterrupt): it names
 * no client channel. The list is nvgpu's and NVIDIA's whitelists, not what this GPU answers (a second LTC's registers,
 * for one), and a register no unit answers reads 0xbadf.... with a fault; so the bus's fault interrupt stays masked for
 * the reads and what they raised is cleared, as gf100_bus_intr clears it. */
static void regdump_held(struct nvkm_device *device, void *arg)
{
	struct regdump_pass *pass = arg;
	struct tgm_regdump_how *how = pass->how;
	struct nvkm_runl *runl = NULL;
	u64 start = 0;
	u32 faults = 0;

	nvkm_runl_foreach(runl, device->fifo)
	{
		mutex_lock(&runl->mutex);
	}
	pass->bus_en = nvkm_rd32(device, BUS_INTR_EN_0);
	nvkm_wr32(device, BUS_INTR_EN_0, pass->bus_en & ~BUS_INTR_PRI_FAULTS);
	pass->bus = nvkm_rd32(device, BUS_INTR_0);
	pass->save0 = nvkm_rd32(device, PRI_TIMEOUT_SAVE_0);
	pass->save1 = nvkm_rd32(device, PRI_TIMEOUT_SAVE_1);
	how->engine_before = nvkm_rd32(device, FIFO_ENGINE_STATUS0);
	how->ctx_before = nvkm_rd32(device, FECS_CURRENT_CTX);
	start = nvxnu_nanotime();
	for (size_t i = 0; i < TGM_REGDUMP_COUNT; i++)
		pass->values[i] = regdump_read(device, pass, tgm_regdump_offsets[i]);
	how->read_us = (u32)((nvxnu_nanotime() - start) / 1000);
	how->engine_after = nvkm_rd32(device, FIFO_ENGINE_STATUS0);
	how->ctx_after = nvkm_rd32(device, FECS_CURRENT_CTX);
	faults = nvkm_rd32(device, BUS_INTR_0) & BUS_INTR_PRI_FAULTS & ~pass->bus;
	if (faults) {
		how->bus_fault = nvkm_rd32(device, PRI_TIMEOUT_SAVE_0) & 0x00fffffc;
		nvkm_wr32(device, PRI_TIMEOUT_SAVE_0, 0);
		nvkm_wr32(device, BUS_INTR_0, faults);
	}
	nvkm_wr32(device, BUS_INTR_EN_0, pass->bus_en);
	nvkm_runl_foreach(runl, device->fifo)
	{
		mutex_unlock(&runl->mutex);
	}
}

/* values: TGM_REGDUMP_COUNT words. 0, or -EBUSY before M3. */
static int regdump_read_all(u32 *values, struct tgm_regdump_how *how)
{
	struct regdump_pass pass = {.values = values, .how = how};
	int ret = 0;

	*how = (struct tgm_regdump_how){0};
	ret = tgm_gpu_held(regdump_held, &pass);
	if (!ret)
		how->badf = tgm_regdump_badf(values);
	return ret;
}

size_t tgm_regdump_bytes(void)
{
	return TGM_REGDUMP_BYTES;
}

int tgm_regdump(char *buf, size_t cap, size_t *len)
{
	struct tgm_regdump_how how;
	u32 *values = NULL;
	int ret = 0;

	*len = 0;
	if (cap < TGM_REGDUMP_BYTES)
		return -ENOSPC;
	values = kcalloc(TGM_REGDUMP_COUNT, sizeof(*values), GFP_KERNEL);
	if (!values)
		return -ENOMEM;
	ret = regdump_read_all(values, &how);
	if (!ret)
		*len = tgm_regdump_format(buf, cap, values, &how);
	kfree(values);
	return ret;
}
#endif
