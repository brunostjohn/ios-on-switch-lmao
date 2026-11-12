/* SPDX-License-Identifier: MIT */
/* The give-up of a writer promise nobody keeps (SwitchGpuWriterPromise.h): the one place a promise is given up
 * (switch_promise_giveup_site_test fails on a second).
 *
 * A reader parks on a pending promise word in one of three ways: a GPU job that acquires it, once kicked; a CPU reader
 * asleep behind it; the display's direct present watching it. The first one to park arms one call,
 * SWITCH_GPU_GIVEUP_NS later. That call is cancelled when the word is kept (the keeping job's interrupt pass finds it
 * moved) or when the last reader leaves. It reads the word again whenever it runs, so a missed cancel costs a run,
 * never a give-up. Nothing is armed while no reader is parked.
 *
 * The call does not give up a promise whose job is submitted but queued behind another job of its channel. It waits for
 * that job ahead through the channel's interrupt pass and judges the promised job from that job's completion. While it
 * waits it is that channel's waiter for the hang rule (tgm_chan_check_progress's two strikes): a job ahead that never
 * completes is the hang rule's to recover, never this rule's. Otherwise the promise is given up: marked abandoned, its
 * word raised by a plain store with the fence page's owner unchanged, one line naming its maker, its first reader and
 * the time since that reader parked. A present of the surface is then dropped and the panel keeps the frame before it. */
#pragma once
#include <stdint.h>
#include "SwitchGpuWriterPromise.h"

#define SWITCH_GPU_GIVEUP_NS (1000ull * 1000 * 1000)
#define SWITCH_GPU_GIVEUPS 32
#define SWITCH_GPU_GIVEUP_READERS 4
#define SWITCH_GPU_GIVEUP_KEPT 16

/* A GPU job: its channel's uid and the first seqno it releases (its acquires come before it) */
struct switch_gpu_giveup_job {
	uint32_t uid, first;
};

struct switch_gpu_giveup {
	uint32_t offset; /* 0: free (no promise word is at offset 0) */
	uint32_t value, owner;
	uint32_t waiters; /* CPU readers and the direct present: each leaves itself */
	uint32_t readers; /* the GPU readers in reader[] */
	uint8_t overflow; /* more GPU readers than reader[] holds: parked until the word moves */
	uint8_t kept;     /* job is the promised job */
	uint8_t display;  /* the direct present is a waiter */
	struct switch_gpu_giveup_job reader[SWITCH_GPU_GIVEUP_READERS];
	struct switch_gpu_giveup_job job;
	uint32_t job_last;
	/* deferred: the promised job's channel fence and the seqno of the job ahead of it */
	const volatile uint32_t *ahead;
	uint32_t ahead_seqno;
	uint64_t parked_ns, due_ns;
	const char *who;
	char waiter[17];
};

struct switch_gpu_giveup_kept {
	uint32_t offset, value, last;
	struct switch_gpu_giveup_job job;
};

struct switch_gpu_giveups {
	struct switch_gpu_giveup entry[SWITCH_GPU_GIVEUPS];
	struct switch_gpu_giveup_kept kept[SWITCH_GPU_GIVEUP_KEPT];
	uint32_t kept_next;
	uint32_t used;     /* entries in use: the interrupt pass's look is one load while none is */
	uint64_t armed_ns; /* when the call is due; 0: not armed (the call's simple lock) */
};

/* Where the promised job is (switch_gpu_giveup_ops.job_ahead) */
enum {
	SWITCH_GPU_GIVEUP_HEAD = 0,   /* everything before it on its channel is complete */
	SWITCH_GPU_GIVEUP_BEHIND = 1, /* a job ahead of it is not */
	SWITCH_GPU_GIVEUP_GONE = 2,   /* its channel died */
};

/* The kernel under the table's lock (tgm_live_lock) */
struct switch_gpu_giveup_ops {
	const struct switch_gpu_promise_fence *fence;
	/* the reader's first seqno not complete on a live channel: its acquire has not passed */
	int (*reader_parked)(const struct switch_gpu_giveup_job *reader);
	/* BEHIND: *fence and *seqno name the job ahead, whose interrupt is asked for, and *check_ns the channel's next hang
	 * check by its rule, this caller its waiter */
	int (*job_ahead)(const struct switch_gpu_giveup_job *job, const volatile uint32_t **fence,
			 uint32_t *seqno, uint64_t *check_ns);
	/* a channel asked for the interrupt at `seqno`: a quiet job raises none unless asked */
	void (*need)(uint32_t uid, uint32_t seqno);
	/* each step of a give-up (SWITCH_GPU_PROMISE_RAISING, then RAISED or KEPT_FIRST) */
	void (*stepped)(uint32_t offset, uint32_t value, int step, void *entry);
	/* the word moved while the direct present still waited on it: a job it never heard of kept it */
	void (*moved)(uint32_t offset, uint32_t value);
};

/* The call's arming, under its simple lock */
struct switch_gpu_giveup_call {
	void (*after)(uint64_t ns);
	void (*cancel)(void);
};

static inline int switch_gpu_giveup_pending(const struct switch_gpu_promise_fence *fence,
					    uint32_t offset, uint32_t value, uint32_t owner)
{
	const volatile uint32_t *at = fence->word(offset);

	return at && fence->owner(offset) == owner &&
	       (int32_t)(__atomic_load_n(at, __ATOMIC_ACQUIRE) - value) < 0;
}

static inline void switch_gpu_giveup_free(struct switch_gpu_giveups *g, struct switch_gpu_giveup *e)
{
	__atomic_store_n(&e->offset, 0u, __ATOMIC_RELEASE);
	__atomic_store_n(&e->ahead, (const volatile uint32_t *)0, __ATOMIC_RELAXED);
	__atomic_store_n(&g->used, g->used - 1, __ATOMIC_RELEASE);
}

static inline void switch_gpu_giveup_add(struct switch_gpu_giveup *e,
					 const struct switch_gpu_giveup_job *reader, int display)
{
	if (!reader) {
		e->waiters++;
		e->display |= (uint8_t)(display != 0);
		return;
	}
	for (uint32_t i = 0; i < e->readers; ++i)
		if (e->reader[i].uid == reader->uid && e->reader[i].first == reader->first)
			return;
	if (e->readers == SWITCH_GPU_GIVEUP_READERS)
		e->overflow = 1;
	else
		e->reader[e->readers++] = *reader;
}

/* The call's simple lock held: a kept promise remembered for a reader that parks on it after keptPromises ran */
static inline void switch_gpu_giveup_remember(struct switch_gpu_giveups *g, uint32_t offset,
					      uint32_t value,
					      const struct switch_gpu_giveup_job *job,
					      uint32_t last)
{
	struct switch_gpu_giveup_kept *k = &g->kept[g->kept_next++ % SWITCH_GPU_GIVEUP_KEPT];

	k->offset = offset;
	k->value = value;
	k->last = last;
	k->job = *job;
}

/* The call's simple lock held: the promised job remembered for the entry's promise, taken into it; 1 if found */
static inline int switch_gpu_giveup_recall(const struct switch_gpu_giveups *g,
					   struct switch_gpu_giveup *e)
{
	for (uint32_t i = 0; i < SWITCH_GPU_GIVEUP_KEPT; ++i) {
		const struct switch_gpu_giveup_kept *k = &g->kept[i];

		if (k->offset == e->offset && k->value == e->value && k->job.uid) {
			e->kept = 1;
			e->job = k->job;
			e->job_last = k->last;
			return 1;
		}
	}
	return 0;
}

/* A reader parks on the promise at `now`: a GPU job (reader), or a waiter that leaves itself. The first one arms the
 * entry SWITCH_GPU_GIVEUP_NS out. The entry, -1 when there is nothing to wait for (the word moved, or its page has
 * another owner), -28 while SWITCH_GPU_GIVEUPS other promises are waited for. */
static inline int switch_gpu_giveup_park(struct switch_gpu_giveups *g,
					 const struct switch_gpu_giveup_ops *ops, uint32_t offset,
					 uint32_t value, const char *waiter, const char *who,
					 const struct switch_gpu_giveup_job *reader, int display,
					 uint64_t now)
{
	const uint32_t owner = ops->fence->owner(offset);
	struct switch_gpu_giveup *e = 0;
	int free = -1;

	if (!offset || !switch_gpu_giveup_pending(ops->fence, offset, value, owner))
		return -1;
	for (int i = 0; i < SWITCH_GPU_GIVEUPS; ++i) {
		e = &g->entry[i];
		if (e->offset &&
		    !switch_gpu_giveup_pending(ops->fence, e->offset, e->value, e->owner))
			switch_gpu_giveup_free(g, e);
		if (e->offset == offset && e->value == value && e->owner == owner) {
			switch_gpu_giveup_add(e, reader, display);
			return i;
		}
		if (!e->offset && free < 0)
			free = i;
	}
	if (free < 0)
		return -28;
	e = &g->entry[free];
	__builtin_memset(e, 0, sizeof(*e));
	e->value = value;
	e->owner = owner;
	e->parked_ns = now;
	e->due_ns = now + SWITCH_GPU_GIVEUP_NS;
	e->who = who;
	for (uint32_t i = 0; waiter && i < sizeof(e->waiter) - 1 && waiter[i]; ++i)
		e->waiter[i] = waiter[i];
	switch_gpu_giveup_add(e, reader, display);
	__atomic_store_n(&g->used, g->used + 1, __ATOMIC_RELEASE);
	__atomic_store_n(&e->offset, offset, __ATOMIC_RELEASE);
	return free;
}

/* A waiter that parked without a GPU job goes on. The entry is dropped once no reader is left. */
static inline void switch_gpu_giveup_leave(struct switch_gpu_giveups *g, uint32_t offset,
					   uint32_t value, int display)
{
	for (int i = 0; i < SWITCH_GPU_GIVEUPS; ++i) {
		struct switch_gpu_giveup *e = &g->entry[i];

		if (e->offset != offset || e->value != value || !e->waiters)
			continue;
		e->waiters--;
		if (display)
			e->display = 0;
		if (!e->waiters && !e->readers && !e->overflow)
			switch_gpu_giveup_free(g, e);
		return;
	}
}

/* The promise's job is submitted (remembered first): an entry judges it from now on, and its end is asked for, so the
 * interrupt pass after it finds the word moved and cancels the call. */
static inline void switch_gpu_giveup_kept(struct switch_gpu_giveups *g,
					  const struct switch_gpu_giveup_ops *ops, uint32_t offset,
					  uint32_t value, const struct switch_gpu_giveup_job *job,
					  uint32_t last)
{
	for (int i = 0; i < SWITCH_GPU_GIVEUPS; ++i) {
		struct switch_gpu_giveup *e = &g->entry[i];

		if (e->offset != offset || e->value != value || e->kept)
			continue;
		e->kept = 1;
		e->job = *job;
		e->job_last = last;
		ops->need(job->uid, last);
	}
}

/* Whether a reader is still parked; GPU readers whose acquire passed, or whose channel died, are dropped */
static inline int switch_gpu_giveup_waited(const struct switch_gpu_giveup_ops *ops,
					   struct switch_gpu_giveup *e)
{
	uint32_t kept = 0;

	for (uint32_t i = 0; i < e->readers; ++i)
		if (ops->reader_parked(&e->reader[i]))
			e->reader[kept++] = e->reader[i];
	e->readers = kept;
	return e->waiters || e->readers || e->overflow;
}

/* The earliest due among the entries still waited on, entries whose word moved dropped; 0: none */
static inline uint64_t switch_gpu_giveup_next(struct switch_gpu_giveups *g,
					      const struct switch_gpu_giveup_ops *ops)
{
	uint64_t next = 0;

	for (int i = 0; i < SWITCH_GPU_GIVEUPS; ++i) {
		struct switch_gpu_giveup *e = &g->entry[i];

		if (!e->offset)
			continue;
		if (!switch_gpu_giveup_pending(ops->fence, e->offset, e->value, e->owner)) {
			switch_gpu_giveup_free(g, e);
			continue;
		}
		if (!next || (int64_t)(e->due_ns - next) < 0)
			next = e->due_ns;
	}
	return next;
}

static inline void switch_gpu_giveup_defer(struct switch_gpu_giveup *e,
					   const volatile uint32_t *fence, uint32_t seqno,
					   uint64_t check_ns, uint64_t now)
{
	__atomic_store_n(&e->ahead_seqno, seqno, __ATOMIC_RELAXED);
	__atomic_store_n(&e->ahead, fence, __ATOMIC_RELEASE);
	e->due_ns = now + check_ns;
}

/* The promised job's turn came at `now`: it gets the whole time from there */
static inline void switch_gpu_giveup_restart(struct switch_gpu_giveup *e, uint64_t now)
{
	__atomic_store_n(&e->ahead, (const volatile uint32_t *)0, __ATOMIC_RELAXED);
	e->due_ns = now + SWITCH_GPU_GIVEUP_NS;
}

/* 1: the entry is to be given up at `now`; 0: it waits on, its due set again */
static inline int switch_gpu_giveup_judge(const struct switch_gpu_giveup_ops *ops,
					  struct switch_gpu_giveup *e, uint64_t now)
{
	const volatile uint32_t *fence = 0;
	uint32_t seqno = 0;
	uint64_t check = 0;
	int where = SWITCH_GPU_GIVEUP_HEAD;
	const volatile uint32_t *ahead = __atomic_load_n(&e->ahead, __ATOMIC_ACQUIRE);

	if (ahead && (int32_t)(__atomic_load_n(ahead, __ATOMIC_ACQUIRE) - e->ahead_seqno) >= 0) {
		switch_gpu_giveup_restart(e, now);
		return 0;
	}
	if ((int64_t)(now - e->due_ns) < 0)
		return 0;
	if (!e->kept)
		return 1;
	where = ops->job_ahead(&e->job, &fence, &seqno, &check);
	if (where == SWITCH_GPU_GIVEUP_BEHIND) {
		switch_gpu_giveup_defer(e, fence, seqno, check, now);
		return 0;
	}
	if (where == SWITCH_GPU_GIVEUP_HEAD && ahead) {
		switch_gpu_giveup_restart(e, now);
		return 0;
	}
	return 1;
}

/* The call runs at `now`: each entry still waited on is judged, and those due are given up. */
static inline void switch_gpu_giveup_run(struct switch_gpu_giveups *g,
					 const struct switch_gpu_giveup_ops *ops, uint64_t now)
{
	for (int i = 0; i < SWITCH_GPU_GIVEUPS; ++i) {
		struct switch_gpu_giveup *e = &g->entry[i];

		if (!e->offset)
			continue;
		if (!switch_gpu_giveup_pending(ops->fence, e->offset, e->value, e->owner)) {
			if (e->display)
				ops->moved(e->offset, e->value);
			switch_gpu_giveup_free(g, e);
			continue;
		}
		if (!switch_gpu_giveup_waited(ops, e)) {
			switch_gpu_giveup_free(g, e);
			continue;
		}
		if (!switch_gpu_giveup_judge(ops, e, now))
			continue;
		switch_gpu_promise_give_up(ops->fence, e->offset, e->value, e->owner, ops->stepped,
					   e);
		switch_gpu_giveup_free(g, e);
	}
}

/* The call's simple lock held: armed for `next` (0: cancelled), from `now` */
static inline void switch_gpu_giveup_arm_locked(struct switch_gpu_giveups *g,
						const struct switch_gpu_giveup_call *call,
						uint64_t next, uint64_t now)
{
	if (!next) {
		if (g->armed_ns)
			call->cancel();
		g->armed_ns = 0;
		return;
	}
	if (g->armed_ns == next)
		return;
	g->armed_ns = next;
	call->after((int64_t)(next - now) > 0 ? next - now : 0);
}

enum {
	SWITCH_GPU_GIVEUP_PASS_NONE = 0,
	SWITCH_GPU_GIVEUP_PASS_RUN =
		1, /* a deferred promise's job ahead completed, or a word the direct present waits on
					 * moved: the call runs now */
	SWITCH_GPU_GIVEUP_PASS_CANCEL = 2, /* every word waited on moved: the call is cancelled */
};

/* The end of a GPU interrupt pass: plain loads, no lock. An entry being filled or dropped meanwhile is read again under
 * the call's lock before a cancel, and its writer arms the call under that lock after it. */
static inline int switch_gpu_giveup_pass(const struct switch_gpu_giveups *g,
					 const struct switch_gpu_promise_fence *fence)
{
	int waited = 0;

	if (!__atomic_load_n(&g->used, __ATOMIC_ACQUIRE))
		return SWITCH_GPU_GIVEUP_PASS_NONE;
	for (int i = 0; i < SWITCH_GPU_GIVEUPS; ++i) {
		const struct switch_gpu_giveup *e = &g->entry[i];
		const uint32_t offset = __atomic_load_n(&e->offset, __ATOMIC_ACQUIRE);
		const volatile uint32_t *ahead = __atomic_load_n(&e->ahead, __ATOMIC_ACQUIRE);

		if (!offset)
			continue;
		if (!switch_gpu_giveup_pending(fence, offset,
					       __atomic_load_n(&e->value, __ATOMIC_RELAXED),
					       __atomic_load_n(&e->owner, __ATOMIC_RELAXED))) {
			if (__atomic_load_n(&e->display, __ATOMIC_RELAXED))
				return SWITCH_GPU_GIVEUP_PASS_RUN;
			continue;
		}
		if (ahead && (int32_t)(__atomic_load_n(ahead, __ATOMIC_ACQUIRE) -
				       __atomic_load_n(&e->ahead_seqno, __ATOMIC_RELAXED)) >= 0)
			return SWITCH_GPU_GIVEUP_PASS_RUN;
		waited = 1;
	}
	return waited ? SWITCH_GPU_GIVEUP_PASS_NONE : SWITCH_GPU_GIVEUP_PASS_CANCEL;
}
