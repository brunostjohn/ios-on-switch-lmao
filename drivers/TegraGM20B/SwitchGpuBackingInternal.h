/* SPDX-License-Identifier: MIT */
#pragma once
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include "SwitchGpuBacking.h"

struct switch_gpu_backing_pages {
	switch_gpu_backing_pages *older;
	uint64_t *addresses;
	uint32_t count;
};

struct switch_gpu_backing_access {
	switch_gpu_backing_token token;
	uint64_t content;
	uint32_t access;
	bool noted;  // its failure was already logged, or needs no log
};

struct switch_gpu_backing {
	switch_gpu_backing *next;
	switch_gpu_backing *retiring_next;
	IOLock *lock;
	uint64_t id, generation, content;
	uint64_t clean_start;
	uint32_t refs, surface_id;
	switch_gpu_backing_pages *pages;
	void *owner;
	void (*release_owner)(void *);
	switch_gpu_backing_layout layout;
	switch_gpu_backing_access *accesses;
	uint32_t access_count, access_capacity;
	uint32_t cpu_readers, cpu_writers, dirty;
	bool initial_clean, retiring;
};

struct switch_gpu_cpu_lease {
	switch_gpu_backing *backing;
	uint64_t offset, bytes;
	uint32_t access;
};

const switch_gpu_backing_engine *switch_gpu_backing_engine_ops(uint32_t engine);
bool switch_gpu_backing_same_timeline(const switch_gpu_backing_token &a,
				     const switch_gpu_backing_token &b);
bool switch_gpu_backing_later(const switch_gpu_backing_token &a,
			      const switch_gpu_backing_token &b);
void switch_gpu_backing_prune_locked(switch_gpu_backing *backing);
void switch_gpu_backing_drop_token(const switch_gpu_backing_token &token);
extern "C" void switch_gpu_backing_cache(const uint64_t *pages, uint32_t count,
					 uint64_t offset, uint64_t bytes, int operation);
extern "C" int switch_gpu_backing_prepare_retirement(void);
extern "C" void switch_gpu_backing_schedule_retirement(void);
extern "C" void switch_gpu_backing_retire(void);
