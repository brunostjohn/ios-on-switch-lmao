/* SPDX-License-Identifier: MIT */
/* A fence-only present whose one GM20B producer is still running at the offer, completed with no present job. The
 * offer arms the watch, asks the producer's channel for the interrupt at its seqno (a quiet job raises none unasked,
 * SwitchGpuQuiet.h), then looks at the producer once more, so a pass that ran just before the arm is not missed.
 * The producer channel's interrupt pass makes it due with plain loads (nvkm's event lock is held there), and the pass's
 * end, in thread context, delivers it. A kill, a channel's teardown, a semantic wait and a producer that ended with its
 * writes still in the L2 go through the present's watch work item, which may sleep. The deadline takes none: it follows
 * work the asked interrupt is queued behind, and panics for one that was due and never came (gpu_present_deadline.c). */
#pragma once
#include <stdint.h>
#include "SwitchGpuBacking.h"

enum {
	SWITCH_GPU_PRESENT_WATCH_IDLE = 0,
	SWITCH_GPU_PRESENT_WATCH_ARMED = 1,
	SWITCH_GPU_PRESENT_WATCH_DUE = 2,
	SWITCH_GPU_PRESENT_WATCH_CLAIMED = 3,
};

enum {
	SWITCH_GPU_PRESENT_WATCH_PENDING = 0,
	SWITCH_GPU_PRESENT_WATCH_VISIBLE = 1,
	/* Completed with its writes possibly still in the L2: a fence-only present job flushes them first. */
	SWITCH_GPU_PRESENT_WATCH_HIDDEN = 2,
	SWITCH_GPU_PRESENT_WATCH_FAILED = 3,
};

struct switch_gpu_present_watch {
	uint32_t state;
	struct switch_gpu_backing_token token;
};

typedef int (*switch_gpu_present_watch_status)(const struct switch_gpu_backing_token *);

static inline int switch_gpu_present_watchable(const struct switch_gpu_backing_token *tokens,
					       uint32_t count)
{
	return count == 1 && tokens && tokens[0].engine == SWITCH_GPU_ENGINE_GM20B;
}

static inline void switch_gpu_present_watch_arm(struct switch_gpu_present_watch *watch,
						const struct switch_gpu_backing_token *token)
{
	watch->token = *token;
	__atomic_store_n(&watch->state, SWITCH_GPU_PRESENT_WATCH_ARMED, __ATOMIC_SEQ_CST);
}

static inline int switch_gpu_present_watch_armed(const struct switch_gpu_present_watch *watch)
{
	return __atomic_load_n(&watch->state, __ATOMIC_ACQUIRE) != SWITCH_GPU_PRESENT_WATCH_IDLE;
}

/* The channel whose uid and fence these are reached the watched seqno: due. Plain loads only. */
static inline int switch_gpu_present_watch_fenced(struct switch_gpu_present_watch *watch,
						  uint64_t channel_uid, uint32_t completed)
{
	uint32_t armed = SWITCH_GPU_PRESENT_WATCH_ARMED;
	if (__atomic_load_n(&watch->state, __ATOMIC_ACQUIRE) != SWITCH_GPU_PRESENT_WATCH_ARMED ||
	    watch->token.queue != (uint32_t)channel_uid ||
	    watch->token.reset_generation != (channel_uid >> 32) + 1 ||
	    (int32_t)(completed - (uint32_t)watch->token.value) < 0)
		return 0;
	return __atomic_compare_exchange_n(&watch->state, &armed, SWITCH_GPU_PRESENT_WATCH_DUE, 0,
					   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

/* Due, and its producer's writes in DRAM with nothing semantic left: the pass's end takes it. */
static inline int switch_gpu_present_watch_take_visible(struct switch_gpu_present_watch *watch,
							switch_gpu_present_watch_status status,
							switch_gpu_present_watch_status external)
{
	uint32_t due = SWITCH_GPU_PRESENT_WATCH_DUE;
	if (__atomic_load_n(&watch->state, __ATOMIC_ACQUIRE) != SWITCH_GPU_PRESENT_WATCH_DUE ||
	    status(&watch->token) != 1 || external(&watch->token) != 1)
		return 0;
	return __atomic_compare_exchange_n(&watch->state, &due, SWITCH_GPU_PRESENT_WATCH_CLAIMED, 0,
					   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

/* Armed or due: whatever the producer came to. completed and external are switch_gpu_backing_token_physical_status
 * without and with visibility. Completion is read before visibility: an External job releases its visibility word
 * ahead of its fence, so a completion seen first cannot hide a visibility that came with it. */
static inline int switch_gpu_present_watch_claim(struct switch_gpu_present_watch *watch,
						 switch_gpu_present_watch_status completed,
						 switch_gpu_present_watch_status external)
{
	uint32_t from = __atomic_load_n(&watch->state, __ATOMIC_ACQUIRE);
	int found;
	int done;
	if (from != SWITCH_GPU_PRESENT_WATCH_ARMED && from != SWITCH_GPU_PRESENT_WATCH_DUE)
		return SWITCH_GPU_PRESENT_WATCH_PENDING;
	done = completed(&watch->token);
	if (!done)
		return SWITCH_GPU_PRESENT_WATCH_PENDING;
	if (done < 0) {
		found = SWITCH_GPU_PRESENT_WATCH_FAILED;
	} else {
		const int visible = external(&watch->token);
		found = visible < 0   ? SWITCH_GPU_PRESENT_WATCH_FAILED
			: visible ? SWITCH_GPU_PRESENT_WATCH_VISIBLE
				  : SWITCH_GPU_PRESENT_WATCH_HIDDEN;
	}
	if (!__atomic_compare_exchange_n(&watch->state, &from, SWITCH_GPU_PRESENT_WATCH_CLAIMED, 0,
					 __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		return SWITCH_GPU_PRESENT_WATCH_PENDING;
	return found;
}

static inline void switch_gpu_present_watch_reset(struct switch_gpu_present_watch *watch)
{
	__atomic_store_n(&watch->state, SWITCH_GPU_PRESENT_WATCH_IDLE, __ATOMIC_RELEASE);
}
