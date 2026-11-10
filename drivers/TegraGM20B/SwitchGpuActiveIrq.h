/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>

#define SWITCH_GPU_IRQ_ACTIVE (UINT64_C(1) << 32)

static inline int switch_gpu_irq_arm(uint64_t *state, uint32_t sequence)
{
	const uint64_t previous =
		__atomic_exchange_n(state, SWITCH_GPU_IRQ_ACTIVE | sequence, __ATOMIC_ACQ_REL);
	return !(previous & SWITCH_GPU_IRQ_ACTIVE);
}

static inline int switch_gpu_irq_complete(uint64_t *state, uint32_t completed)
{
	uint64_t seen = __atomic_load_n(state, __ATOMIC_ACQUIRE);
	if (!(seen & SWITCH_GPU_IRQ_ACTIVE))
		return 1;
	if ((int32_t)(completed - (uint32_t)seen) < 0)
		return 0;
	return __atomic_compare_exchange_n(state, &seen, (uint32_t)seen, 0, __ATOMIC_ACQ_REL,
					   __ATOMIC_ACQUIRE);
}
