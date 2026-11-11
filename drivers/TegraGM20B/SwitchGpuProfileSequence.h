/* SPDX-License-Identifier: MIT */
/* The push profile's slowest submission of a window, push by push in order (gpu.c "push profile"): each push's class,
 * its pass's colour target 0 in thousands of pixels, its draws and interlock enables, and its GPU time. With the jobs
 * line's batches it attributes a frame's GPU time pass by pass. */
#ifndef SWITCH_GPU_PROFILE_SEQUENCE_H
#define SWITCH_GPU_PROFILE_SEQUENCE_H
#include <stdint.h>
#include "TegraGM20BGpu.h"

#define SWITCH_GPU_PROF_SEQ 64

struct switch_gpu_prof_push {
	uint8_t cls, ilk;
	uint16_t draws, kpx;
	uint32_t us;
};

/* The interrupt pass offers, the window's line takes; `gen` is odd while a slower one is copied in. */
struct switch_gpu_prof_seq {
	uint32_t gen, first, last, n, pushes;
	uint64_t ns;
	struct switch_gpu_prof_push push[SWITCH_GPU_PROF_SEQ];
};

static inline void switch_gpu_prof_seq_offer(struct switch_gpu_prof_seq *slow, const struct switch_gpu_prof_seq *run)
{
	const uint32_t n = run->n < SWITCH_GPU_PROF_SEQ ? run->n : SWITCH_GPU_PROF_SEQ;

	if (run->ns <= __atomic_load_n(&slow->ns, __ATOMIC_RELAXED))
		return;
	__atomic_fetch_add(&slow->gen, 1, __ATOMIC_ACQ_REL);
	slow->first = run->first;
	slow->last = run->last;
	slow->n = n;
	slow->pushes = run->pushes;
	for (uint32_t i = 0; i < n; i++)
		slow->push[i] = run->push[i];
	__atomic_store_n(&slow->ns, run->ns, __ATOMIC_RELAXED);
	__atomic_fetch_add(&slow->gen, 1, __ATOMIC_RELEASE);
}

/* Takes the window's slowest into `out` (out->n 0: none, or one was being copied in) and starts the next window. */
static inline void switch_gpu_prof_seq_take(struct switch_gpu_prof_seq *slow, struct switch_gpu_prof_seq *out)
{
	const uint32_t gen = __atomic_load_n(&slow->gen, __ATOMIC_ACQUIRE);

	out->first = slow->first;
	out->last = slow->last;
	out->n = slow->n < SWITCH_GPU_PROF_SEQ ? slow->n : SWITCH_GPU_PROF_SEQ;
	out->pushes = slow->pushes;
	for (uint32_t i = 0; i < out->n; i++)
		out->push[i] = slow->push[i];
	out->ns = __atomic_exchange_n(&slow->ns, 0, __ATOMIC_RELAXED);
	if ((gen & 1) || __atomic_load_n(&slow->gen, __ATOMIC_ACQUIRE) != gen || !out->ns)
		out->n = 0;
}

/* A push: D draw, C clear, P pass, S sync, X compute, Y copy, 2 2D engine, T state; then for draws and clears its
 * target's kilopixels, p if pitch-linear, m if multisampled, /draws, i and the interlock enables (conflict detection
 * on); then its microseconds. */
static inline int switch_gpu_prof_push_format(char *buf, unsigned long len, const struct switch_gpu_prof_push *p)
{
	static const char other[] = "PSXY2T";

	if (p->cls >= TGM_PC_PASS)
		return snprintf(buf, len, " %c %u", p->cls - TGM_PC_PASS < 6 ? other[p->cls - TGM_PC_PASS] : '?', p->us);
	return snprintf(buf, len, " %c%uk%s%s/%u%s%.0u %u", p->cls < TGM_PC_CLEAR ? 'D' : 'C', p->kpx,
			p->cls & 2 ? "p" : "", p->cls & 1 ? "m" : "", p->draws, p->ilk ? "i" : "", p->ilk, p->us);
}

static inline int switch_gpu_prof_seq_format(char *buf, unsigned long len, const char *owner,
					     const struct switch_gpu_prof_seq *seq)
{
	unsigned long at = 0;
	int put = 0;

	if (!seq->n)
		return 0;
	put = snprintf(buf, len, "%s seqno %u-%u, %llu us:", owner, seq->first, seq->last,
		       (unsigned long long)(seq->ns / 1000));
	at = put > 0 ? (unsigned long)put : 0;
	for (uint32_t i = 0; i < seq->n && at < len; i++) {
		put = switch_gpu_prof_push_format(buf + at, len - at, &seq->push[i]);
		at += put > 0 ? (unsigned long)put : 0;
	}
	if (seq->pushes > seq->n && at < len)
		snprintf(buf + at, len - at, " +%u pushes", seq->pushes - seq->n);
	if (at >= len && len)
		buf[len - 1] = 0;
	return 1;
}

#endif
