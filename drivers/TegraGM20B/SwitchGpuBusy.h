/* SPDX-License-Identifier: MIT */
/* The hardware channels with work the GPU may not have finished: a job's kick sets its channel's bit, and the interrupt
 * pass that sees the channel's fence reach its newest seqno clears it (its teardown too). A high kick and the deadline
 * read the low channels' bits to leave PFIFO's registers alone while no app has work: a channel at the high level is
 * never one a high kick preempts or the hold takes. A bit left set (a quiet job whose end no pass has seen yet) costs
 * the register reads it would have saved.
 *
 * Order: the kick publishes the seqno, then sets its bit; the pass clears the bit, then reads the seqno again and sets
 * the bit back if it moved. A clear that follows the set acquires the seqno the set released, so a kick racing the
 * clear keeps its bit. A high kick that reads the word in that instant finds the app idle, as one that reads PFIFO
 * before the app's doorbell lands does. */
#pragma once
#include <stdint.h>

#define SWITCH_GPU_BUSY_CHIDS 512u /* GM20B's channels (CCSR_CHANNEL__SIZE_1) */
#define SWITCH_GPU_BUSY_WORDS (SWITCH_GPU_BUSY_CHIDS / 64u)

struct switch_gpu_busy {
	uint64_t busy[SWITCH_GPU_BUSY_WORDS];
	uint64_t high[SWITCH_GPU_BUSY_WORDS];
};

/* After the kick published `chid`'s newest seqno: 1 when the channel had no work in flight before it. Always the
 * read-modify-write, no barrier: its release orders the seqno before the bit for the pass that clears it. */
static inline int switch_gpu_busy_kicked(struct switch_gpu_busy *b, uint32_t chid)
{
	const uint64_t bit = UINT64_C(1) << (chid % 64u);

	if (chid >= SWITCH_GPU_BUSY_CHIDS)
		return 0;
	return !(__atomic_fetch_or(&b->busy[chid / 64u], bit, __ATOMIC_SEQ_CST) & bit);
}

/* The pass found the channel's fence at `completed`: its bit goes once that is its newest seqno. */
static inline void switch_gpu_busy_settled(struct switch_gpu_busy *b, uint32_t chid,
					   uint32_t completed, const uint32_t *submitted)
{
	const uint64_t bit = UINT64_C(1) << (chid % 64u);
	uint64_t *word = NULL;

	if (chid >= SWITCH_GPU_BUSY_CHIDS ||
	    __atomic_load_n(submitted, __ATOMIC_RELAXED) != completed)
		return;
	word = &b->busy[chid / 64u];
	if (!(__atomic_load_n(word, __ATOMIC_RELAXED) & bit))
		return;
	__atomic_fetch_and(word, ~bit, __ATOMIC_SEQ_CST);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	if (__atomic_load_n(submitted, __ATOMIC_RELAXED) != completed)
		__atomic_fetch_or(word, bit, __ATOMIC_SEQ_CST);
}

/* The channel is being destroyed: nothing of it runs any more, and its id may be handed out again. */
static inline void switch_gpu_busy_gone(struct switch_gpu_busy *b, uint32_t chid)
{
	if (chid < SWITCH_GPU_BUSY_CHIDS)
		__atomic_fetch_and(&b->busy[chid / 64u], ~(UINT64_C(1) << (chid % 64u)),
				   __ATOMIC_SEQ_CST);
}

/* The scheduler: whether `chid` is at the high level now. */
static inline void switch_gpu_busy_level(struct switch_gpu_busy *b, uint32_t chid, int high)
{
	const uint64_t bit = UINT64_C(1) << (chid % 64u);

	if (chid >= SWITCH_GPU_BUSY_CHIDS)
		return;
	if (high)
		__atomic_fetch_or(&b->high[chid / 64u], bit, __ATOMIC_SEQ_CST);
	else
		__atomic_fetch_and(&b->high[chid / 64u], ~bit, __ATOMIC_SEQ_CST);
}

/* Whether a channel below the high level has work in flight. */
static inline int switch_gpu_busy_low(const struct switch_gpu_busy *b)
{
	uint32_t i = 0;

	for (i = 0; i < SWITCH_GPU_BUSY_WORDS; i++)
		if (__atomic_load_n(&b->busy[i], __ATOMIC_SEQ_CST) &
		    ~__atomic_load_n(&b->high[i], __ATOMIC_RELAXED))
			return 1;
	return 0;
}

/* Whether one of the channels in `mask` (SWITCH_GPU_BUSY_CHIDS bits) has work in flight. */
static inline int switch_gpu_busy_among(const struct switch_gpu_busy *b, const uint64_t *mask)
{
	uint32_t i = 0;

	for (i = 0; i < SWITCH_GPU_BUSY_WORDS; i++)
		if (__atomic_load_n(&b->busy[i], __ATOMIC_SEQ_CST) & mask[i])
			return 1;
	return 0;
}

static inline int switch_gpu_busy_chan(const struct switch_gpu_busy *b, uint32_t chid)
{
	return chid < SWITCH_GPU_BUSY_CHIDS &&
	       ((__atomic_load_n(&b->busy[chid / 64u], __ATOMIC_SEQ_CST) >> (chid % 64u)) & 1u);
}
