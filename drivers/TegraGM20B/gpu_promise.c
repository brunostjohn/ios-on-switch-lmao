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

static void promise_released(uint32_t offset, uint32_t value, void *arg)
{
	const struct promise_hang *hang = arg;

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
