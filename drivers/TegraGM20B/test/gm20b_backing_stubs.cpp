// SPDX-License-Identifier: MIT
#include <stdint.h>
#include "drivers/TegraGM20B/SwitchGpuPathStats.h"
struct switch_gpu_backing;
extern "C" switch_gpu_backing *switch_gpu_backing_find_pa(uint64_t) { return nullptr; }
extern "C" void switch_gpu_backing_release(switch_gpu_backing *) {}

extern "C" void switch_gpu_backing_retain(switch_gpu_backing *) {}

#if SWITCH_GPU_PATH_STATS
extern "C" void switch_gpu_path_taken(enum switch_gpu_path, uint32_t) {}
#endif
