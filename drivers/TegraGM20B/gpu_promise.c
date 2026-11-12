// SPDX-License-Identifier: MIT
// Writer promises (SwitchGpuWriterPromise.h): those a channel waits for on the GPU, as the hang rule sees them, and
// the one a direct present watches (SwitchGpuPresentWatch.h).
#include "gpu_internal.h"

int tgm_chan_note_promise(struct tgm_chan *chan, uint32_t offset, uint32_t value)
{
	return switch_gpu_promise_waits_note(&chan->promise_waits, tgm_fence_word, offset, value);
}

int tgm_chan_waits_promise(const struct tgm_chan *chan)
{
	return switch_gpu_promise_waits_pending(&chan->promise_waits, tgm_fence_word);
}

/* tgm_present.lock held: the present's job parks on an unkept promise, its own acquire's or that of a GM20B producer it
 * waits for (whose channel's acquires include one), so its deadline's first look comes once that wait has been stuck
 * for TGM_STUCK_NS */
int tgm_present_waits_promise(const struct switch_gpu_present_tokens *tokens)
{
	u32 producers[SWITCH_GPU_PRESENT_MAX_DEPENDENCIES];
	u32 count = 0;
	u32 i;
	int waits = 0;

	if (switch_gpu_present_tokens_promised(tokens))
		return 1;
	/* their status may take a timeline's lock, which tgm_live_lock is never held around */
	for (i = 0; i < tokens->count; i++)
		if (tokens->tokens[i].engine == SWITCH_GPU_ENGINE_GM20B &&
		    !switch_gpu_backing_token_status(&tokens->tokens[i]))
			producers[count++] = i;
	mutex_lock(&tgm_live_lock);
	for (i = 0; i < count && !waits; i++) {
		const struct switch_gpu_backing_token *token = &tokens->tokens[producers[i]];
		const struct tgm_chan *chan = live_by_uid(token->queue);

		waits = chan && switch_gpu_quiet_token_on(token, chan->id) && tgm_chan_waits_promise(chan);
	}
	mutex_unlock(&tgm_live_lock);
	return waits;
}

struct promise_hang {
	const struct tgm_chan *chan;
	const char *who;
	u64 waited_ns;
};

#if TGM_STATS
/* Promise words the CPU raised (a cancel, a kill, a close, the hang rule): a reader parked on ACQ_GEQ for one goes on
 * only if the GPU reads the word again after that write, which the present's completion reports once
 * (tgm_promise_was_cpu_raised). */
#define TGM_PROMISE_RAISED 8
static uint64_t promise_raised[TGM_PROMISE_RAISED];
static uint32_t promise_raised_next;

void tgm_promise_cpu_raised(uint32_t offset, uint32_t value)
{
	const uint32_t at =
		__atomic_fetch_add(&promise_raised_next, 1, __ATOMIC_RELAXED) % TGM_PROMISE_RAISED;

	__atomic_store_n(&promise_raised[at], (uint64_t)offset << 32 | value, __ATOMIC_RELEASE);
	switch_gpu_path_taken(SWITCH_GPU_PATH_PROMISE_CPU_RELEASE, 1);
}

int tgm_promise_was_cpu_raised(uint32_t offset, uint32_t value)
{
	const uint64_t raised = (uint64_t)offset << 32 | value;
	u32 i;

	for (i = 0; i < TGM_PROMISE_RAISED; i++)
		if (__atomic_load_n(&promise_raised[i], __ATOMIC_ACQUIRE) == raised)
			return 1;
	return 0;
}

/* The present's job completed after acquiring writer promises on the GPU: logged once for one its promised job's fence
 * released, and once for one the CPU raised (its channel, parked on that ACQ_GEQ, read the word again after the CPU
 * wrote it). A CPU raise older than the last TGM_PROMISE_RAISED reads as the job's. */
void tgm_present_promises_done(const struct switch_gpu_present_tokens *tokens, u64 queued, int copy)
{
	static int logged[2];
	u32 i;

	for (i = 0; i < tokens->count; i++) {
		const struct switch_gpu_backing_token *token = &tokens->tokens[i];
		int cpu = 0;

		if (token->engine != SWITCH_GPU_ENGINE_PROMISE)
			continue;
		cpu = tgm_promise_was_cpu_raised(token->queue, (u32)token->value);
		if (logged[cpu])
			continue;
		logged[cpu] = 1;
		nvxnu_log("tegra-gm20b: first %s present behind a writer promise %s completed %llu ms after its offer "
			  "(fence offset %u value %u)\n",
			  copy ? "copy" : "fence-only",
			  cpu ? "the CPU released (a channel parked on ACQ_GEQ read the word again after a CPU write)"
			      : "its promised job's fence released",
			  (nvxnu_nanotime() - queued) / 1000000, token->queue, (u32)token->value);
	}
}
#endif

static struct switch_gpu_promise_abandoned promise_abandoned;
static void *promise_wake_work;

/* CPU readers asleep behind a promise given up on go on, as after a cancel: thread context, for the backings' locks */
static void promise_wake(void *unused)
{
	(void)unused;
	switch_gpu_backing_promise_sleepers_wake();
}

int tgm_promise_open(void)
{
	if (!promise_wake_work)
		promise_wake_work = nvxnu_work_alloc(promise_wake, NULL);
	return promise_wake_work ? 0 : -ENOMEM;
}

int tgm_promise_abandoned(uint32_t offset, uint32_t value)
{
	return switch_gpu_promise_abandoned_has(&promise_abandoned, offset, value);
}

void tgm_promise_abandon(uint32_t offset, uint32_t value)
{
	switch_gpu_promise_abandoned_add(&promise_abandoned, offset, value);
	nvxnu_work_schedule(promise_wake_work);
	tgm_present_promise_released(offset, value);
}

/* tgm_live_lock held: the process whose connection's fence page holds the word at `offset` */
static const char *promise_maker(uint32_t offset)
{
	struct tgm_chan *chan = NULL;

	list_for_each_entry(chan, &tgm_live_chans, live)
	{
		if (!chan->kernel && ((uintptr_t)chan->fence_cpu - (uintptr_t)tgm_fence_pages.va) /
						 TGM_FENCE_SLOT ==
					 offset / TGM_FENCE_SLOT)
			return chan->owner;
	}
	return "a connection with no channel";
}

/* tgm_live_lock held, the promise abandoned: the writer is the one reported (the reader goes on, and a present of the
 * surface is dropped). */
void tgm_promise_abandoned_report(uint32_t offset, uint32_t value, const char *who, u64 waited_ns)
{
	switch_gpu_path_promise_abandoned(waited_ns);
	nvxnu_log("tegra-gm20b: %s's writer promise at fence offset %u (value %u) was not kept: %s waited %llu ms "
		  "for it; it goes on without the promised writes, and a present of the surface is dropped\n",
		  promise_maker(offset), offset, value, who, waited_ns / 1000000);
}

static void promise_released(uint32_t offset, uint32_t value, int step, void *arg)
{
	const struct promise_hang *hang = arg;

	if (step == SWITCH_GPU_PROMISE_RAISING) {
		tgm_promise_abandon(offset, value);
		return;
	}
	if (step == SWITCH_GPU_PROMISE_KEPT_FIRST) {
		switch_gpu_promise_abandoned_drop(&promise_abandoned, offset, value);
		return;
	}
#if TGM_STATS
	tgm_promise_cpu_raised(offset, value);
#endif
	tgm_promise_abandoned_report(offset, value, hang->chan->owner, hang->waited_ns);
}

/* A pending promise word raised from the CPU: a plain store-release after one more look. The fence pool is mapped
 * write-combined (Normal Non-cacheable), where T210's bus answers an exclusive access (a compare-and-swap, any atomic
 * read-modify-write) with an SError. Interrupts are off so the look and the store are a few instructions apart: only a
 * later promise on the slot, whose job ran between them, could be lowered by the store. */
static int promise_raise(volatile uint32_t *word, uint32_t value)
{
	unsigned long flags;
	int raised = 0;

	local_irq_save(flags);
	if ((s32)(__atomic_load_n(word, __ATOMIC_ACQUIRE) - value) < 0) {
		__atomic_store_n(word, value, __ATOMIC_RELEASE);
		raised = 1;
	}
	local_irq_restore(flags);
	return raised;
}

static const struct switch_gpu_promise_fence promise_fence = {tgm_fence_word, promise_raise};

/* tgm_live_lock held. The fence pool is write-combined on the CPU and volatile (no L2) on the GPU, so the parked
 * channel's PBDMA reads the word from DRAM, which a store-release need not have reached until a DSB completes it. */
uint32_t tgm_chan_release_promises(struct tgm_chan *chan, const char *who, u64 waited_ns)
{
	struct promise_hang hang = {chan, who, waited_ns};
	const uint32_t raised = switch_gpu_promise_waits_release(&chan->promise_waits, &promise_fence,
								 promise_released, &hang);

	if (raised)
		__asm__ volatile("dsb sy" ::: "memory");
	return raised;
}

/* Promises kept lately, under tgm_present.lock: an offer whose snapshot took one just before its job was submitted
 * watches that job. */
#define PRESENT_KEPT 8
struct present_kept {
	u32 offset, value;
	struct switch_gpu_backing_token producer;
};
static struct present_kept present_kept[PRESENT_KEPT];
static u32 present_kept_next;
#if TGM_STATS
static int present_kept_logged;
#endif

/* tgm_present.lock held, the job retained: it takes the promise's place among the present's tokens, as the GM20B
 * producer a watch of it finishes. Not the promise too: the job's fence push releases the word after the fence a pass
 * may already have seen. */
static void present_takes(const struct switch_gpu_backing_token *producer)
{
	switch_gpu_backing_token_release(&tgm_present.tokens.tokens[0]);
	tgm_present.tokens.tokens[0] = *producer;
}

/* tgm_present.lock held, the offer's one token a writer promise: its job as *kept if it is submitted already, else
 * nothing (the watch waits PROMISED). */
void tgm_present_promise_watch(struct switch_gpu_backing_token *kept)
{
	const struct switch_gpu_backing_token promise = tgm_present.tokens.tokens[0];
	u32 i;

	for (i = 0; i < PRESENT_KEPT; i++) {
		const struct present_kept *k = &present_kept[i];

		if (k->producer.engine && k->offset == promise.queue && k->value == (u32)promise.value &&
		    !switch_gpu_backing_token_retain(&k->producer)) {
			present_takes(&k->producer);
			switch_gpu_path_promise_direct(SWITCH_GPU_PATH_DIRECT_PROMISED);
			switch_gpu_path_promise_direct(SWITCH_GPU_PATH_DIRECT_KEPT);
			*kept = k->producer;
			return;
		}
	}
}

/* SubmitJob, the job that keeps the promise submitted: a watch waiting for it watches the job, whose fence push
 * releases the word after its fence, as the nonstall filter's view (SwitchGpuPresentFilter.cpp) and as the producer
 * whose asked interrupt's pass completes the watch. */
void tgm_present_promise_kept(uint32_t offset, uint32_t value, const struct switch_gpu_backing_token *producer)
{
	int watched = 0;

	mutex_lock(&tgm_present.lock);
	present_kept[present_kept_next++ % PRESENT_KEPT] = (struct present_kept){offset, value, *producer};
	if (switch_gpu_present_watch_promised(&tgm_present.watch, offset, value) &&
	    !switch_gpu_backing_token_retain(producer)) {
		if (switch_gpu_present_watch_kept(&tgm_present.watch, offset, value, producer)) {
			present_takes(producer);
			switch_gpu_path_promise_direct(SWITCH_GPU_PATH_DIRECT_KEPT);
			/* before the re-arm, as the offer attaches before its arm: the job's end then runs the display's flip
			 * from the nonstall filter, and a pass claiming the watch detaches the view after this */
			tgm_present_filter_attach(producer);
			switch_gpu_present_watch_rearm(&tgm_present.watch);
			watched = 1;
		} else {
			switch_gpu_backing_token_release(producer);
		}
	}
	mutex_unlock(&tgm_present.lock);
	if (!watched)
		return;
#if TGM_STATS
	if (!__atomic_exchange_n(&present_kept_logged, 1, __ATOMIC_RELAXED))
		nvxnu_log("tegra-gm20b: first direct present of a frame behind a writer promise watches the job "
			  "that keeps it, with no present job\n");
#endif
	tgm_chan_need_tokens(producer, 1);
	if (switch_gpu_backing_token_physical_status(producer, 0))
		nvxnu_work_schedule(tgm_present.watch_work);
}

/* A promise released or given up on from the CPU: a watch still waiting for its job goes to the work item. */
void tgm_present_promise_released(uint32_t offset, uint32_t value)
{
	if (switch_gpu_present_watch_promised(&tgm_present.watch, offset, value) && tgm_present.watch_work)
		nvxnu_work_schedule(tgm_present.watch_work);
}
