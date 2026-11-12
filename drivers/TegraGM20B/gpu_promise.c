// SPDX-License-Identifier: MIT
// Writer promises a channel waits for on the GPU (SwitchGpuWriterPromise.h), as the hang rule sees them.
#include "gpu_internal.h"

int tgm_chan_note_promise(struct tgm_chan *chan, uint32_t offset, uint32_t value)
{
	return switch_gpu_promise_waits_note(&chan->promise_waits, tgm_fence_word, offset, value);
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

static void promise_released(uint32_t offset, uint32_t value, void *arg)
{
	const struct promise_hang *hang = arg;

#if TGM_STATS
	tgm_promise_cpu_raised(offset, value);
#endif
	nvxnu_log("tegra-gm20b: GPU hang, releasing the writer promise at fence offset %u (value "
		  "%u) %s "
		  "(channel %u) waited for: %s waited %llu ms, its maker's command buffer never "
		  "ran\n",
		  offset, value, hang->chan->owner, hang->chan->chid, hang->who,
		  hang->waited_ns / 1000000);
}

uint32_t tgm_chan_release_promises(struct tgm_chan *chan, const char *who, u64 waited_ns)
{
	struct promise_hang hang = {chan, who, waited_ns};

	return switch_gpu_promise_waits_release(&chan->promise_waits, tgm_fence_word,
						promise_released, &hang);
}
