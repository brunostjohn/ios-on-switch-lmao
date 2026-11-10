/* SPDX-License-Identifier: MIT */
#pragma once
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include "SwitchGpuBacking.h"
#include "SwitchGpuWriterRanges.h"

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
	switch_gpu_backing *retiring_next, *expiry_next, *expiry_prev;
	IOLock *lock;
	uint64_t id, generation, content, host_epoch, host_visible_epoch;
	uint64_t clean_start, retirement_deadline, host_acquire_epoch;
	switch_gpu_backing_token host_acquire;
	uint32_t refs, surface_id;
	switch_gpu_backing_pages *pages;
	void *owner;
	void (*release_owner)(void *);
	void *gpu_memory;
	void (*release_gpu_memory)(void *);
	uint32_t gpu_memory_count, gpu_memory_flags;
	uint64_t *cpu_current;
	uint64_t cpu_current_content;
	uint32_t cpu_current_pages;
    switch_gpu_writer_range *writers, *writer_next;
    uint32_t writer_count, writer_capacity, writer_next_count, writer_next_capacity;
    bool cpu_tracking, writers_pending, writers_precise;
	switch_gpu_backing_layout layout;
	switch_gpu_backing_access *accesses;
	switch_gpu_cpu_lease *cpu_leases;
	uint32_t access_count, access_capacity;
	uint32_t cpu_readers, cpu_writers, concurrent_readers, concurrent_writers, dirty;
	uint32_t gpu_admission_readers, gpu_admission_writers;
	bool initial_clean, retiring;
};

struct switch_gpu_cpu_lease {
	switch_gpu_cpu_lease *next;
	switch_gpu_backing *backing;
	uint64_t offset, bytes;
	uint32_t access;
	switch_gpu_backing_token producer;
};

const switch_gpu_backing_engine *switch_gpu_backing_engine_ops(uint32_t engine);
bool switch_gpu_backing_same_timeline(const switch_gpu_backing_token &a,
				     const switch_gpu_backing_token &b);
bool switch_gpu_backing_later(const switch_gpu_backing_token &a,
			      const switch_gpu_backing_token &b);
void switch_gpu_backing_prune_locked(switch_gpu_backing *backing);
void switch_gpu_backing_note_failure_locked(const switch_gpu_backing *backing,
					    switch_gpu_backing_access &access, int status);
void switch_gpu_backing_drop_token(const switch_gpu_backing_token &token);
extern "C" void switch_gpu_backing_cache(const uint64_t *pages, uint32_t count,
					 uint64_t offset, uint64_t bytes, int operation);
extern "C" int switch_gpu_backing_prepare_retirement(void);
extern "C" void switch_gpu_backing_schedule_retirement(void);
extern "C" void switch_gpu_backing_retire(void);

extern "C" uint64_t switch_gpu_backing_time_ns(void);
extern "C" void switch_gpu_backing_arm_expiry(uint64_t deadline_ns);
extern "C" void switch_gpu_backing_expire(void);

const switch_gpu_cpu_lease *switch_gpu_backing_find_lease_locked(const switch_gpu_backing *backing,
		const switch_gpu_cpu_lease *lease);

void switch_gpu_backing_sort_uses(switch_gpu_backing_use *uses, uint32_t count);

bool switch_gpu_backing_cpu_current_prepare_locked(switch_gpu_backing *);
void switch_gpu_backing_cpu_cache_locked(switch_gpu_backing *, uint64_t offset, uint64_t bytes, int operation);
void switch_gpu_backing_cpu_current_written_locked(switch_gpu_backing *, uint64_t offset, uint64_t bytes);
void switch_gpu_backing_cpu_written_locked(switch_gpu_backing *, uint64_t offset, uint64_t bytes);
int switch_gpu_backing_writer_prepare_locked(switch_gpu_backing *, const switch_gpu_backing_token &);
void switch_gpu_backing_writer_commit_locked(switch_gpu_backing *, const switch_gpu_backing_token &);
void switch_gpu_backing_writer_abort_locked(switch_gpu_backing *);
void switch_gpu_backing_writer_free_locked(switch_gpu_backing *);
bool switch_gpu_backing_writer_prefix_locked(const switch_gpu_backing *, const switch_gpu_backing_token &,
    uint64_t offset, uint64_t bytes);
