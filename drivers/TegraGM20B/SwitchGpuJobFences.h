/* SPDX-License-Identifier: MIT */
/* A job's internal fences (TegraGM20BPushFence): one submission carries a command buffer's work batches, and the GPU
 * releases the channel's next seqno at the end of each batch, as when each batch was its own submission. The job's
 * resources are resolved and owned once, against the seqno of its own fence; the internal ones keep the channel's
 * completed seqno moving while the job runs (the stuck rule of TegraGM20BRecover.h counts from the last one). */
#pragma once
#include <stdint.h>
#include "TegraGM20BABI.h"
#include "TegraGM20BGpu.h"

/* An internal fence's kernel words: an end stamp, the WFI semaphore release, the GET report before its nonstall
 * interrupt, the interrupt, and the GET report of the entry that carries them (5 + 5 + 5 + 2 + 5). */
#define SWITCH_GPU_FENCE_POINT_DWORDS 32

/* 0 and the count, or -22 for a fence after an incomplete push or the job's last, -7 for more than the ABI allows. */
static inline int switch_gpu_job_fences(const struct tgm_push *pushes, uint32_t count, uint32_t *fences)
{
	uint32_t n = 0;
	for (uint32_t i = 0; i < count; ++i) {
		if (!pushes[i].fence)
			continue;
		if (i + 1 == count || pushes[i].incomplete)
			return -22;
		++n;
	}
	if (n > TegraGM20BMaxJobFences)
		return -7;
	*fences = n;
	return 0;
}

struct switch_gpu_job_emitter {
	void *context;
	void (*entry)(void *context, uint32_t push);
	void (*fence)(void *context, uint32_t seqno);
};

/* The user entries in order, an internal fence releasing the next seqno after each marked push. Returns the seqno
 * the job's own fence releases: `submitted` + the internal fences + 1. */
static inline uint32_t switch_gpu_job_emit(const struct tgm_push *pushes, uint32_t count, uint32_t submitted,
					   const struct switch_gpu_job_emitter *emitter)
{
	uint32_t seqno = submitted + 1;
	for (uint32_t i = 0; i < count; ++i) {
		emitter->entry(emitter->context, i);
		if (pushes[i].fence)
			emitter->fence(emitter->context, seqno++);
	}
	return seqno;
}
