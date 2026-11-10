/* SPDX-License-Identifier: MIT */
#ifndef SWITCH_GPU_BACKING_H
#define SWITCH_GPU_BACKING_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
	SWITCH_GPU_ENGINE_GM20B = 1,
	SWITCH_GPU_ENGINE_CE = 2,
	SWITCH_GPU_ENGINE_VIC = 3,
	SWITCH_GPU_ENGINE_NVDEC = 4,
	SWITCH_GPU_ENGINE_DISPLAY = 5,
	SWITCH_GPU_ENGINE_COUNT = 6,
	SWITCH_GPU_BACKING_READ = 1u << 0,
	SWITCH_GPU_BACKING_WRITE = 1u << 1,
	SWITCH_GPU_BACKING_CPU_AVOID_SYNC = 1u << 2,
	SWITCH_GPU_BACKING_CPU_DISCARD = 1u << 3,
	SWITCH_GPU_BACKING_CPU_NO_CACHE = 1u << 4,
	SWITCH_GPU_BACKING_CPU_CONCURRENT = 1u << 5,
	SWITCH_GPU_BACKING_GPU_DIRTY = 1u << 0,
	SWITCH_GPU_BACKING_CPU_DIRTY = 1u << 1,
	SWITCH_GPU_BACKING_PITCH = 0,
	SWITCH_GPU_BACKING_BLOCK_LINEAR = 1,
	SWITCH_GPU_BACKING_MAX_PLANES = 4,
};

struct switch_gpu_backing;

struct switch_gpu_backing_token {
	uint64_t value;
	uint64_t reset_generation;
	uint32_t engine;
	uint32_t queue;
};

struct switch_gpu_backing_plane {
	uint64_t offset;
	uint64_t bytes;
	uint32_t width, height, pitch, bytes_per_element;
	uint32_t layout, block_height_log2;
};

struct switch_gpu_backing_layout {
	uint32_t format, plane_count;
	uint32_t color_matrix, color_range, transfer, primaries;
	struct switch_gpu_backing_plane planes[SWITCH_GPU_BACKING_MAX_PLANES];
};

struct switch_gpu_backing_info {
	uint64_t allocation_id, allocation_generation, content_generation;
	const uint64_t *pages;
	uint32_t page_count, surface_id, cpu_locks, dirty;
	struct switch_gpu_backing_layout layout;
};

/* The page vector remains valid until the lease is released. Registration takes the owner's supplied reference on
 * success; release_owner must release the actual pinned allocation, not only a surface wrapper. */
struct switch_gpu_backing *switch_gpu_backing_register(const uint64_t *pages, uint32_t count,
						     uint32_t surface_id, void *owner,
						     void (*release_owner)(void *));
struct switch_gpu_backing *switch_gpu_backing_find_pa(uint64_t first_page);
struct switch_gpu_backing *switch_gpu_backing_find_surface(uint32_t surface_id);
void switch_gpu_backing_retain(struct switch_gpu_backing *backing);
void switch_gpu_backing_release(struct switch_gpu_backing *backing);
void switch_gpu_backing_describe(struct switch_gpu_backing *backing,
				 struct switch_gpu_backing_info *info);
int switch_gpu_backing_set_layout(struct switch_gpu_backing *backing,
				  const struct switch_gpu_backing_layout *layout);

/* status/wait: 1 completed, 0 pending, negative errno on failed work. retain_token pins the timeline's completion
 * record through channel destruction/reset; release_token balances it. value_bits is 32 or 64. */
struct switch_gpu_backing_engine {
	int (*status)(const struct switch_gpu_backing_token *);
	int (*wait)(const struct switch_gpu_backing_token *);
	int (*retain_token)(const struct switch_gpu_backing_token *);
	void (*release_token)(const struct switch_gpu_backing_token *);
	uint32_t value_bits;
};
int switch_gpu_backing_register_engine(uint32_t engine,
				      const struct switch_gpu_backing_engine *operations);
int switch_gpu_backing_token_status(const struct switch_gpu_backing_token *token);
int switch_gpu_backing_token_wait(const struct switch_gpu_backing_token *token);

struct switch_gpu_backing_use {
	struct switch_gpu_backing *backing;
	uint32_t access, reserved;
};

struct switch_gpu_backing_transaction {
	struct switch_gpu_backing_use *uses;
	uint32_t count, locked, prepared;
	struct switch_gpu_backing_token completion;
};

/* begin sorts/merges the caller's use array and locks its backings in allocation order. Keep those locks only through
 * dependency encoding and publication; every success must end with commit or abort. No token can be lost at commit. */
int switch_gpu_backing_begin(struct switch_gpu_backing_use *uses, uint32_t count,
			     const struct switch_gpu_backing_token *completion,
			     struct switch_gpu_backing_transaction *transaction);
int switch_gpu_backing_dependencies(const struct switch_gpu_backing_transaction *transaction,
				    struct switch_gpu_backing_token *out, uint32_t capacity,
				    uint32_t *needed);
void switch_gpu_backing_commit(struct switch_gpu_backing_transaction *transaction);
void switch_gpu_backing_abort(struct switch_gpu_backing_transaction *transaction);

/* Readers wait for writers; writers wait for all users. DISCARD is valid only for a complete cache-line overwrite.
 * release cleans written lines before advancing content_generation and making the allocation available to engines. */
int switch_gpu_backing_cpu_acquire(struct switch_gpu_backing *backing, uint64_t offset,
				   uint64_t bytes, uint32_t access);
int switch_gpu_backing_cpu_release(struct switch_gpu_backing *backing, uint64_t offset,
				   uint64_t bytes, uint32_t access);
int switch_gpu_backing_cpu_publish(struct switch_gpu_backing *backing, uint64_t offset, uint64_t bytes);
void switch_gpu_backing_cpu_cancel(struct switch_gpu_backing *backing, uint32_t access);
int switch_gpu_backing_wait(struct switch_gpu_backing *backing, uint32_t access);

#ifdef __cplusplus
}
#endif
#endif
