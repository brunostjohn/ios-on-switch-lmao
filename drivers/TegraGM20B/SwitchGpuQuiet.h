/* SPDX-License-Identifier: MIT */
/* Quiet job ends (TegraGM20BJobQuiet): a job whose end nobody waits for releases its seqno without the nonstall
 * interrupt. A waiter asks for one before it sleeps (switch_gpu_quiet_need): a job kicked after the ask raises it at its
 * own end, and one kicked before is followed by a wake entry (a NON_STALL_INTERRUPT of its own GPFIFO entry) that the
 * channel's owner pushes under its submission order, so the interrupt comes at or after the seqno asked for.
 *
 * Order: an ask publishes its seqno, then reads what is announced; a job's fence reads the asks, then publishes what it
 * announces. Each side's fence between the two makes one of them see the other, so no ask is left behind both. */
#pragma once
#include <stdint.h>

/* announced: the newest seqno a pushed nonstall interrupt follows (the owner writes it); wanted: the newest seqno a
 * waiter asked for an interrupt at */
struct switch_gpu_quiet {
	uint32_t announced;
	uint32_t wanted;
};

static inline void switch_gpu_quiet_init(struct switch_gpu_quiet *q, uint32_t submitted)
{
	q->announced = submitted;
	q->wanted = submitted;
}

/* At a job's fence, by the submission's owner: whether the fence raises the nonstall interrupt. */
static inline int switch_gpu_quiet_announces(struct switch_gpu_quiet *q, int quiet)
{
	if (!quiet)
		return 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	return (int32_t)(__atomic_load_n(&q->wanted, __ATOMIC_RELAXED) -
			 __atomic_load_n(&q->announced, __ATOMIC_RELAXED)) > 0;
}

/* Before the kick of a fence or wake entry that raises the interrupt behind `seqno`. */
static inline void switch_gpu_quiet_announce(struct switch_gpu_quiet *q, uint32_t seqno)
{
	__atomic_store_n(&q->announced, seqno, __ATOMIC_RELEASE);
}

/* A waiter of `seqno`, before it tests the fence and sleeps: 1 when no pushed interrupt follows that seqno yet, so the
 * owner has to be asked for a wake entry. */
static inline int switch_gpu_quiet_need(struct switch_gpu_quiet *q, uint32_t seqno)
{
	uint32_t seen = __atomic_load_n(&q->wanted, __ATOMIC_RELAXED);

	while ((int32_t)(seqno - seen) > 0 &&
	       !__atomic_compare_exchange_n(&q->wanted, &seen, seqno, 0, __ATOMIC_SEQ_CST,
					    __ATOMIC_RELAXED))
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	return (int32_t)(seqno - __atomic_load_n(&q->announced, __ATOMIC_ACQUIRE)) > 0;
}

/* The owner, under its submission order, with `submitted` the channel's newest seqno: whether a wake entry is owed.
 * An ask past `submitted` belongs to a job not kicked yet, whose own fence sees it; the newest kicked job still gets
 * the entry, as an ask the max replaced may be for it. */
static inline int switch_gpu_quiet_wake_due(const struct switch_gpu_quiet *q, uint32_t submitted)
{
	const uint32_t announced = __atomic_load_n(&q->announced, __ATOMIC_RELAXED);

	return (int32_t)(submitted - announced) > 0 &&
	       (int32_t)(__atomic_load_n(&q->wanted, __ATOMIC_ACQUIRE) - announced) > 0;
}
