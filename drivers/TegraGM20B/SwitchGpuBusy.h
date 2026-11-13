/* SPDX-License-Identifier: MIT */
/* Which hardware channels may have work the GPU has not finished, from two words per channel with one writer each: the
 * newest seqno kicked (the channel's submission owner) and the newest the interrupt pass saw done (the pass). `low`
 * lists the live channels below the high level, a high kick's possible victims; only a channel's creation, level change
 * and teardown write it. A high kick and the deadline read these to leave PFIFO's registers alone while no app has work.
 *
 * Readers load the done word before the kicked one, so a stale done word (a quiet job no pass has seen end) reads as
 * work in flight and costs the register reads it would have saved. A reader in the instant of a kick finds the app
 * idle, as one that reads PFIFO before the app's doorbell lands does. */
#pragma once
#include <stdint.h>

#define SWITCH_GPU_BUSY_CHIDS 512u /* GM20B's channels (CCSR_CHANNEL__SIZE_1) */
#define SWITCH_GPU_BUSY_WORDS (SWITCH_GPU_BUSY_CHIDS / 64u)

struct switch_gpu_busy {
	uint32_t kicked[SWITCH_GPU_BUSY_CHIDS];
	uint32_t done[SWITCH_GPU_BUSY_CHIDS];
	uint64_t low[SWITCH_GPU_BUSY_WORDS];
};

/* The channel's owner, kicking the job that ends at `seqno` */
static inline void switch_gpu_busy_kick(struct switch_gpu_busy *b, uint32_t chid, uint32_t seqno)
{
	if (chid < SWITCH_GPU_BUSY_CHIDS) {
		__atomic_store_n(&b->kicked[chid], seqno, __ATOMIC_RELEASE);
	}
}

/* The interrupt pass, finding the channel's fence at `seqno` */
static inline void switch_gpu_busy_done(struct switch_gpu_busy *b, uint32_t chid, uint32_t seqno)
{
	if (chid < SWITCH_GPU_BUSY_CHIDS) {
		__atomic_store_n(&b->done[chid], seqno, __ATOMIC_RELEASE);
	}
}

static inline int switch_gpu_busy_chan(const struct switch_gpu_busy *b, uint32_t chid)
{
	if (chid >= SWITCH_GPU_BUSY_CHIDS) {
		return 0;
	}
	const uint32_t done = __atomic_load_n(&b->done[chid], __ATOMIC_ACQUIRE);
	return __atomic_load_n(&b->kicked[chid], __ATOMIC_ACQUIRE) != done ? 1 : 0;
}

/* Whether one of the channels in `mask` (SWITCH_GPU_BUSY_CHIDS bits) has work in flight. */
static inline int switch_gpu_busy_among(const struct switch_gpu_busy *b, const uint64_t *mask)
{
	uint32_t i = 0;

	for (i = 0; i < SWITCH_GPU_BUSY_WORDS; i++) {
		uint64_t bits = __atomic_load_n(&mask[i], __ATOMIC_ACQUIRE);

		while (bits != 0u) {
			const uint32_t chid = i * 64u + (uint32_t)__builtin_ctzll(bits);

			if (switch_gpu_busy_chan(b, chid) != 0) {
				return 1;
			}
			bits &= bits - 1u;
		}
	}
	return 0;
}

/* Whether a live channel below the high level has work in flight. */
static inline int switch_gpu_busy_low(const struct switch_gpu_busy *b)
{
	return switch_gpu_busy_among(b, b->low);
}

/* A channel's creation, with its fence at `seqno` and nothing in flight */
static inline void switch_gpu_busy_open(struct switch_gpu_busy *b, uint32_t chid, uint32_t seqno)
{
	if (chid < SWITCH_GPU_BUSY_CHIDS) {
		__atomic_store_n(&b->done[chid], seqno, __ATOMIC_RELAXED);
		__atomic_store_n(&b->kicked[chid], seqno, __ATOMIC_RELEASE);
	}
}

/* Setup paths, which several channels' creations, level changes and teardowns may reach at once: whether `chid` is a
 * live channel below the high level (0 at its teardown). */
static inline void switch_gpu_busy_level(struct switch_gpu_busy *b, uint32_t chid, int low)
{
	const uint64_t bit = UINT64_C(1) << (chid % 64u);

	if (chid >= SWITCH_GPU_BUSY_CHIDS) {
		return;
	}
	if (low != 0) {
		__atomic_fetch_or(&b->low[chid / 64u], bit, __ATOMIC_SEQ_CST);
	} else {
		__atomic_fetch_and(&b->low[chid / 64u], ~bit, __ATOMIC_SEQ_CST);
	}
}
