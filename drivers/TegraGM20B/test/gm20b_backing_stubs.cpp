// SPDX-License-Identifier: MIT
#include <stdint.h>
struct switch_gpu_backing;
extern "C" switch_gpu_backing *switch_gpu_backing_find_pa(uint64_t) { return nullptr; }
extern "C" void switch_gpu_backing_release(switch_gpu_backing *) {}

extern "C" void switch_gpu_backing_retain(switch_gpu_backing *) {}
