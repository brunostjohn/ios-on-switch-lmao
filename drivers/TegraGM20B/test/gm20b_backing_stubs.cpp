// SPDX-License-Identifier: MIT
#include <stdint.h>
#include "drivers/TegraGM20B/SwitchGpuPathStats.h"
#include "drivers/TegraGM20B/SwitchGpuBacking.h"
uint64_t gBackingContent;
extern "C" void switch_gpu_backing_describe(switch_gpu_backing *, switch_gpu_backing_info *info)
{
	*info = {};
	info->content_generation = gBackingContent;
}
extern "C" switch_gpu_backing *switch_gpu_backing_find_pa(uint64_t) { return nullptr; }
extern "C" void switch_gpu_backing_release(switch_gpu_backing *) {}

extern "C" void switch_gpu_backing_retain(switch_gpu_backing *) {}

#if SWITCH_GPU_PATH_STATS
extern "C" void switch_gpu_path_taken(enum switch_gpu_path, uint32_t) {}
#endif
