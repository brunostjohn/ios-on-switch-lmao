/* SPDX-License-Identifier: MIT */
#pragma once
#include "SwitchGpuClock.h"

static u32 clock_ceiling_rate(u32 requested)
{
	u32 best = 0, rate = 0, i = 0;
	u32 limit = requested && requested < idle.maximum_khz ? requested : idle.maximum_khz;
	for (i = 0; (rate = tegra_gm20b_dvfs_khz(i)); ++i)
		if (rate <= limit && rate >= idle.idle_khz && rate > best &&
		    !(idle.vco_khz % (2 * rate)) && idle.vco_khz / (2 * rate) <= 31)
			best = rate;
	return best;
}

int switch_gpu_clock_get(struct switch_gpu_clock_state *state)
{
	if (!state)
		return -EINVAL;
	*state = (struct switch_gpu_clock_state){0, 0, 0, 0};
	if (!idle.clk)
		return -ENODEV;
	nvxnu_mutex_lock(idle.clk);
	state->maximum_khz = idle.maximum_khz;
	state->minimum_khz = idle.idle_khz;
	state->ceiling_khz = idle.run_khz;
	state->requested_khz = idle.requested_khz;
	nvxnu_mutex_unlock(idle.clk);
	return READ_ONCE(idle.clock_available) ? 0 : -EIO;
}

int switch_gpu_clock_ceiling(uint32_t requested_khz, uint32_t *applied_khz)
{
	u32 rate = 0, pl = 0, target_pl = 0;
	int result = 0;
	if (!applied_khz)
		return -EINVAL;
	*applied_khz = 0;
	if (!idle.clk || !READ_ONCE(idle.clock_available))
		return -ENODEV;
	gate_enter();
	nvxnu_mutex_lock(idle.clk);
	*applied_khz = idle.run_khz;
	rate = clock_ceiling_rate(requested_khz);
	if (!READ_ONCE(idle.clock_available) || !READ_ONCE(idle.on)) {
		result = -EIO;
	} else if (!rate) {
		result = -ERANGE;
	} else {
		pl = idle.vco_khz / (2 * rate);
		target_pl = READ_ONCE(idle.low) ? idle.idle_pl : pl;
		if ((nvkm_rd32(gpu.device, GPCPLL_COEFF_R) & 0xffff) != idle.vco_coeff ||
		    pl_switch(target_pl)) {
			WRITE_ONCE(idle.clock_available, 0);
			WRITE_ONCE(idle.on, 0);
			result = -EIO;
			*applied_khz = (u32)nvkm_clk_read(gpu.device->clk, nv_clk_src_gpc);
			nvxnu_log("tegra-gm20b: GPU ceiling change failed; current %u kHz\n",
				  *applied_khz);
		} else {
			idle.run_pl = pl;
			idle.run_khz = rate;
			idle.requested_khz = requested_khz;
			*applied_khz = rate;
		}
	}
	nvxnu_mutex_unlock(idle.clk);
	gate_leave();
	return result;
}

int clock_ceiling_restore(void)
{
	int result = 0;
	if (!idle.clk || !READ_ONCE(idle.clock_available))
		return 0;
	nvxnu_mutex_lock(idle.clk);
	if ((nvkm_rd32(gpu.device, GPCPLL_COEFF_R) & 0xffff) != idle.vco_coeff ||
	    pl_switch(idle.run_pl))
		result = -EIO;
	nvxnu_mutex_unlock(idle.clk);
	return result;
}
