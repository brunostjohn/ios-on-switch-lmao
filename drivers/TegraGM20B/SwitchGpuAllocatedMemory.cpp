// SPDX-License-Identifier: MIT
#include "SwitchGpuAllocatedMemory.h"

#include <kern/thread.h>

namespace
{
// The backings' retirement work while it destroys one batch (switch_gpu_backing_retire): the canonical memories it
// releases share one L2 invalidate (tgm_mem_del_later). Only that work item's thread defers; others release at once.
decltype(current_thread()) gRetireThread;
tgm_mem *gRetired;

int allocatedMemoryCreate(const uint64_t *pages, uint32_t count, void **out, uint32_t *flags)
{
	tgm_mem *memory = nullptr;
	const int result = tgm_mem_new_exclusive(nullptr, pages, count, &memory);
	*out = memory;
	*flags = tgm_mem_can_compress(memory) ? SWITCH_GPU_BACKING_MEMORY_FIXED : 0;
	return result;
}

int allocatedMemorySmmuCreate(const uint64_t *pages, uint32_t count, void **out, uint32_t *flags)
{
	tgm_mem *memory = nullptr;
	const int result = tgm_mem_new_exclusive_smmu(nullptr, pages, count, &memory);
	*out = memory;
	*flags = (tgm_mem_can_compress(memory) ? SWITCH_GPU_BACKING_MEMORY_FIXED : 0) |
		 (tgm_mem_is_smmu(memory) ? SWITCH_GPU_BACKING_MEMORY_SMMU : 0);
	return result;
}

int allocatedMemoryBigCreate(const uint64_t *pages, uint32_t count, void **out, uint32_t *flags)
{
	tgm_mem *memory = nullptr;
	const int result = tgm_mem_new_smmu_big(nullptr, pages, count, &memory);
	*out = memory;
	*flags = tgm_mem_is_smmu(memory) ? SWITCH_GPU_BACKING_MEMORY_SMMU : 0;
	return result;
}

int allocatedMemoryDirectFallbackCreate(const uint64_t *pages, uint32_t count, void **out,
					uint32_t *flags)
{
	tgm_mem *memory = nullptr;
	const int result = tgm_mem_new(nullptr, pages, count, &memory);
	*out = memory;
	*flags = 0;
	return result;
}
}  // namespace

extern "C" void switch_gpu_allocated_memory_destroy(void *object)
{
	auto *memory = static_cast<tgm_mem *>(object);
	if (__atomic_load_n(&gRetireThread, __ATOMIC_RELAXED) == current_thread())
		tgm_mem_del_later(&memory, &gRetired);
	else
		tgm_mem_del(&memory);
}

extern "C" void switch_gpu_memory_retire_begin(void)
{
	__atomic_store_n(&gRetireThread, current_thread(), __ATOMIC_RELAXED);
}

extern "C" void switch_gpu_memory_retire_end(void)
{
	__atomic_store_n(&gRetireThread, nullptr, __ATOMIC_RELAXED);
	tgm_mem *later = gRetired;
	gRetired = nullptr;
	tgm_mem_del_finish(later);
}

extern "C" int switch_gpu_backing_tgm_mem_direct(struct switch_gpu_backing *backing, uint32_t count,
						 struct tgm_mem **out)
{
	void *object = nullptr;
	const int result =
		switch_gpu_backing_memory_acquire(backing, count, allocatedMemoryCreate,
						  switch_gpu_allocated_memory_destroy, &object);
	*out = static_cast<tgm_mem *>(object);
	return result;
}

extern "C" int switch_gpu_backing_tgm_mem(struct switch_gpu_backing *backing, uint32_t count,
					  struct tgm_mem **out)
{
	void *object = nullptr;
	const int result = switch_gpu_backing_memory_acquire_smmu(
		backing, count, allocatedMemorySmmuCreate, allocatedMemoryDirectFallbackCreate,
		switch_gpu_allocated_memory_destroy, &object);
	*out = static_cast<tgm_mem *>(object);
	return result;
}

extern "C" int switch_gpu_backing_tgm_mem_big(struct switch_gpu_backing *backing, uint32_t count,
					      struct tgm_mem **out)
{
	void *object = nullptr;
	const int result = switch_gpu_backing_memory_acquire_smmu(
		backing, count, allocatedMemoryBigCreate, allocatedMemoryDirectFallbackCreate,
		switch_gpu_allocated_memory_destroy, &object);
	*out = static_cast<tgm_mem *>(object);
	return result;
}

extern "C" int switch_gpu_backing_tgm_mem_existing(struct switch_gpu_backing *backing,
						   uint32_t count, struct tgm_mem **out)
{
	void *object = nullptr;
	const int result = switch_gpu_backing_memory_existing(
		backing, count, switch_gpu_allocated_memory_destroy, &object);
	*out = static_cast<tgm_mem *>(object);
	return result;
}

extern "C" int switch_gpu_backing_tgm_mem_existing_or_direct(struct switch_gpu_backing *backing,
							     uint32_t count, struct tgm_mem **out)
{
	void *object = nullptr;
	const int result = switch_gpu_backing_memory_existing_or_direct(
		backing, count, switch_gpu_allocated_memory_destroy, &object);
	*out = static_cast<tgm_mem *>(object);
	return result;
}
