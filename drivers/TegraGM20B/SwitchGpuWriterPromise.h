/* SPDX-License-Identifier: MIT */
/* Writer promises (TegraGM20BJobABI.h method 34). The raw runtime counts a command buffer as scheduled before its jobs
 * reach the kernel only while it waits for a shared event; before it does, it promises its writes of the IOSurfaces it
 * names. A promise is a word of its connection's fence page and a value: each surface it names keeps a ref to it, and a
 * present or another connection's job that uses the surface acquires the word on the GPU until the value is reached.
 * A reader that cannot acquire it (a CPU lock, the display's CPU copy, VIC) waits for the promised job's submission,
 * then for its fence. The job that keeps the promise releases it on the GPU at its end; a cancel, a kill or the
 * destruction of the channel it was kept on and the connection's close release it from the CPU, and the hang rule
 * releases one a stuck reader still waits for, so nothing waits for a promise nobody keeps. */
#pragma once
#include <stdint.h>
#include "SwitchGpuBacking.h"

enum {
	SWITCH_GPU_PROMISE_FREE = 0,
	SWITCH_GPU_PROMISE_MADE = 1, /* made, no job of it submitted yet */
	SWITCH_GPU_PROMISE_KEPT = 2, /* its job submitted: released at the job's end */
};

/* A surface's ref to a pending promise: the word as the kernel reads it, its byte offset in the fence pool (the GPU's
 * view through every address space's fence pages) and the value that releases it. */
struct switch_gpu_promise_ref {
	const volatile uint32_t *word;
	uint32_t offset, value;
};

static inline int switch_gpu_promise_ref_released(const struct switch_gpu_promise_ref *ref)
{
	return (int32_t)(__atomic_load_n(ref->word, __ATOMIC_ACQUIRE) - ref->value) >= 0;
}

/* Released refs go first; 0, or -28 with SWITCH_GPU_BACKING_PROMISES pending. */
static inline int switch_gpu_promise_ref_add(struct switch_gpu_promise_ref *refs, uint32_t *count,
					     const struct switch_gpu_promise_ref *ref)
{
	uint32_t kept = 0;

	for (uint32_t i = 0; i < *count; ++i)
		if (!switch_gpu_promise_ref_released(&refs[i]))
			refs[kept++] = refs[i];
	*count = kept;
	if (kept == SWITCH_GPU_BACKING_PROMISES)
		return -28;
	refs[(*count)++] = *ref;
	return 0;
}

static inline void switch_gpu_promise_ref_drop(struct switch_gpu_promise_ref *refs, uint32_t *count,
					       uint32_t offset)
{
	uint32_t kept = 0;

	for (uint32_t i = 0; i < *count; ++i)
		if (refs[i].offset != offset)
			refs[kept++] = refs[i];
	*count = kept;
}

/* The pending refs a user of the surface waits for: their offsets and values, at most `capacity`; the count, or -28 if
 * more are pending. A connection's own promises (offsets in [own_first, own_end)) are never its jobs' to wait for: its
 * runtime orders its own command buffers, and a later job of a held command buffer waiting for an earlier one's promise
 * on the same channel would never run. */
static inline int switch_gpu_promise_ref_pending(const struct switch_gpu_promise_ref *refs,
						 uint32_t count, uint32_t own_first,
						 uint32_t own_end, uint32_t *offsets,
						 uint32_t *values, uint32_t capacity)
{
	uint32_t n = 0;

	for (uint32_t i = 0; i < count; ++i) {
		if ((refs[i].offset >= own_first && refs[i].offset < own_end) ||
		    switch_gpu_promise_ref_released(&refs[i]))
			continue;
		if (n == capacity)
			return -28;
		offsets[n] = refs[i].offset;
		values[n++] = refs[i].value;
	}
	return (int)n;
}

/* A connection's promises: slot i is the fence page's word TegraGM20BPromiseWord + i (`words` from there). */
struct switch_gpu_promise_slot {
	uint32_t value;
	uint8_t state, channel;
};

/* A free slot (one kept and released counts as free), its value one past its word: the slot, or -28. */
static inline int switch_gpu_promise_make(struct switch_gpu_promise_slot *slots,
					  volatile uint32_t *words, uint32_t count)
{
	for (uint32_t i = 0; i < count; ++i) {
		const uint32_t word = __atomic_load_n(&words[i], __ATOMIC_ACQUIRE);

		if (slots[i].state == SWITCH_GPU_PROMISE_MADE ||
		    (slots[i].state == SWITCH_GPU_PROMISE_KEPT &&
		     (int32_t)(word - slots[i].value) < 0))
			continue;
		slots[i].value = word + 1;
		slots[i].state = SWITCH_GPU_PROMISE_MADE;
		return (int)i;
	}
	return -28;
}

/* From the CPU: a cancel, a kill of its channel, the connection's close. 1: the CPU wrote the word (it was pending). */
static inline int switch_gpu_promise_release(struct switch_gpu_promise_slot *slot,
					     volatile uint32_t *word)
{
	int raised = 0;

	if (slot->state == SWITCH_GPU_PROMISE_FREE)
		return 0;
	if ((int32_t)(__atomic_load_n(word, __ATOMIC_ACQUIRE) - slot->value) < 0) {
		__atomic_store_n(word, slot->value, __ATOMIC_RELEASE);
		raised = 1;
	}
	slot->state = SWITCH_GPU_PROMISE_FREE;
	return raised;
}

/* Its job was submitted on `channel` (slot), releasing it at its end. */
static inline void switch_gpu_promise_kept(struct switch_gpu_promise_slot *slot, uint32_t channel)
{
	slot->state = SWITCH_GPU_PROMISE_KEPT;
	slot->channel = (uint8_t)channel;
}

/* The promises a channel's pushes acquired, each offset << 32 | value (0: none; a promise word is never at offset 0).
 * The channel's submitter is the one writer. The hang rule releases those still pending on a channel stuck behind them,
 * as it kills a GPU producer that never ends: a reader of a surface must not hang on an event its maker never
 * signals. */
#define SWITCH_GPU_PROMISE_WAITS 16

struct switch_gpu_promise_waits {
	uint64_t wait[SWITCH_GPU_PROMISE_WAITS];
};

typedef volatile uint32_t *(*switch_gpu_promise_word)(uint32_t offset);

static inline int switch_gpu_promise_pending(switch_gpu_promise_word word, uint64_t wait)
{
	const volatile uint32_t *at = wait ? word((uint32_t)(wait >> 32)) : 0;

	return at && (int32_t)(__atomic_load_n(at, __ATOMIC_ACQUIRE) - (uint32_t)wait) < 0;
}

/* 0, or -28 with SWITCH_GPU_PROMISE_WAITS others pending. */
static inline int switch_gpu_promise_waits_note(struct switch_gpu_promise_waits *waits,
						switch_gpu_promise_word word, uint32_t offset,
						uint32_t value)
{
	const uint64_t wait = (uint64_t)offset << 32 | value;
	int free = -1;

	for (int i = 0; i < SWITCH_GPU_PROMISE_WAITS; ++i) {
		const uint64_t held = __atomic_load_n(&waits->wait[i], __ATOMIC_RELAXED);

		if (held == wait)
			return 0;
		if (free < 0 && !switch_gpu_promise_pending(word, held))
			free = i;
	}
	if (free < 0)
		return -28;
	__atomic_store_n(&waits->wait[free], wait, __ATOMIC_RELAXED);
	return 0;
}

/* Each one still pending released, and `released` told: the count. */
static inline uint32_t switch_gpu_promise_waits_release(
	struct switch_gpu_promise_waits *waits, switch_gpu_promise_word word,
	void (*released)(uint32_t offset, uint32_t value, void *arg), void *arg)
{
	uint32_t count = 0;

	for (int i = 0; i < SWITCH_GPU_PROMISE_WAITS; ++i) {
		const uint64_t wait = __atomic_load_n(&waits->wait[i], __ATOMIC_RELAXED);
		volatile uint32_t *at = wait ? word((uint32_t)(wait >> 32)) : 0;
		uint32_t now = at ? __atomic_load_n(at, __ATOMIC_ACQUIRE) : 0;

		/* the maker's GPU release may land meanwhile: never lower the word under it */
		while (at && (int32_t)(now - (uint32_t)wait) < 0) {
			if (__atomic_compare_exchange_n(at, &now, (uint32_t)wait, 0,
							__ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
				released((uint32_t)(wait >> 32), (uint32_t)wait, arg);
				++count;
				break;
			}
		}
	}
	return count;
}
