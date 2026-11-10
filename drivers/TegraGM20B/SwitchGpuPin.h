/* SPDX-License-Identifier: MIT */
#pragma once
#include <IOKit/IOMemoryDescriptor.h>
#include "SwitchGpuBacking.h"
switch_gpu_backing *switch_gpu_pin_memory(const uint64_t *pages, uint32_t count, uint32_t surface_id,
	IOMemoryDescriptor *memory, bool prepare);
switch_gpu_backing *switch_gpu_pin_chunks(const uint64_t *pages, uint32_t count);
switch_gpu_backing *switch_gpu_pin_cached(const uint64_t *pages, uint32_t count, uint32_t visible_count,
	IOMemoryDescriptor *memory);
