// SPDX-License-Identifier: MIT
#include "SwitchGpuBackingInternal.h"

extern "C" int switch_gpu_backing_cpu_acquire_owned(switch_gpu_backing *backing, uint64_t offset,
		uint64_t bytes, uint32_t access, switch_gpu_cpu_lease **out)
{
	if (!out)
		return -22;
	*out = nullptr;
	if (!(access & SWITCH_GPU_BACKING_CPU_NO_CACHE))
		return -22;
	auto *lease = IONew(switch_gpu_cpu_lease, 1);
	if (!lease)
		return -12;
	const int result = switch_gpu_backing_cpu_acquire(backing, offset, bytes, access);
	if (result) {
		IODelete(lease, switch_gpu_cpu_lease, 1);
		return result;
	}
	*lease = {backing, offset, bytes, access};
	*out = lease;
	return 0;
}

extern "C" int switch_gpu_backing_cpu_release_owned(switch_gpu_cpu_lease **owned, int wrote)
{
	if (!owned || !*owned)
		return -22;
	auto *lease = *owned;
	if ((lease->access & SWITCH_GPU_BACKING_WRITE) && !wrote) {
		switch_gpu_backing_cpu_cancel(lease->backing, lease->access);
	} else {
		const int result = switch_gpu_backing_cpu_release(lease->backing, lease->offset,
			lease->bytes, lease->access);
		if (result)
			return result;
	}
	*owned = nullptr;
	IODelete(lease, switch_gpu_cpu_lease, 1);
	return 0;
}
