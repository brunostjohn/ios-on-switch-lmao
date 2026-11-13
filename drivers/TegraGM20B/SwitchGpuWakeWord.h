/* SPDX-License-Identifier: MIT */
/* The global wake word (nvxnu_wake, nvxnu_idle): every interrupt pass moves its sequence, and only a pass that finds a
 * thread asleep on it wakes one. A sleeper is counted before it looks at the sequence and a waker looks at the count
 * after it moved the sequence, so a sleeper either sees the move or is seen. */
#pragma once
#include <stdint.h>

struct switch_gpu_wake_word {
	uint32_t seq;
	uint32_t sleepers;
};

/* After the waker made a sleeper's condition true: 1 when a thread may sleep on the word, which the caller then wakes. */
static inline int switch_gpu_wake_bump(struct switch_gpu_wake_word *w)
{
	__atomic_fetch_add(&w->seq, 1u, __ATOMIC_SEQ_CST);
	return __atomic_load_n(&w->sleepers, __ATOMIC_SEQ_CST) != 0u ? 1 : 0;
}

/* Around a sleep on w->seq: on before it reads the sequence, off once it is awake. */
static inline void switch_gpu_wake_sleep(struct switch_gpu_wake_word *w, int on)
{
	if (on != 0) {
		__atomic_fetch_add(&w->sleepers, 1u, __ATOMIC_SEQ_CST);
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
	} else {
		__atomic_fetch_sub(&w->sleepers, 1u, __ATOMIC_RELEASE);
	}
}
