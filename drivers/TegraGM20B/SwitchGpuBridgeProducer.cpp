// SPDX-License-Identifier: MIT
#include <IOKit/IOLib.h>
#include "SwitchGpuBridgeProducer.h"
#include "SwitchGpuTimeline.h"
#include "TegraGM20BGpu.h"
#include "../TegraHost1x/TegraHost1x.h"

namespace
{
// Under the bridge's lock. host1x hands out 32 syncpoints and takes none back, and its engines need theirs, so the GPU's
// channels take at most SWITCH_GPU_TIMELINE_SYNCPTS and pass on those whose timelines have gone.
uint32_t made;
uint32_t kept;
}  // namespace

bool switch_gpu_producer_wait(const switch_gpu_backing_token *tokens, uint32_t count,
			      SwitchGpuProducerWait &wait)
{
	bool covered = false;
	for (uint32_t i = 0; i < count; ++i) {
		const auto &token = tokens[i];
		if (switch_gpu_backing_token_external_status(&token) == 1)
			continue;
		if (token.engine != SWITCH_GPU_ENGINE_GM20B)
			return false;
		uint32_t id = 0;
		uint32_t threshold = 0;
		const int asked = switch_gpu_timeline_syncpoint(&token, &id, &threshold);
		if (asked == -61) {
			wait.unassigned = true;
			wait.ask = token;
		}
		if (asked != 1 || (covered && id != wait.syncpoint))
			return false;
		if (!covered || int32_t(threshold - wait.threshold) > 0)
			wait.threshold = threshold;
		wait.syncpoint = id;
		covered = true;
	}
	return covered;
}

void switch_gpu_producer_assign(const SwitchGpuProducerWait &wait)
{
	if (!wait.unassigned)
		return;
	uint32_t id = kept;
	kept = 0;
	if (!id && !switch_gpu_timeline_syncpt_reclaim(&id, 1)) {
		if (made == SWITCH_GPU_TIMELINE_SYNCPTS || tegra_host1x_syncpt_alloc(&id))
			return;
		++made;
	}
	if (switch_gpu_timeline_syncpt_adopt(&wait.ask, id, tegra_host1x_syncpt_read(id)))
		kept = id;
}

bool switch_gpu_producer_running(const switch_gpu_backing_token *tokens, uint32_t count,
				 switch_gpu_backing_token &running)
{
	for (uint32_t i = 0; i < count; ++i)
		if (tokens[i].engine == SWITCH_GPU_ENGINE_GM20B &&
		    !switch_gpu_backing_token_status(&tokens[i])) {
			running = tokens[i];
			return true;
		}
	return false;
}

uint64_t switch_gpu_producer_check(const switch_gpu_backing_token &running)
{
	return tgm_writer_check_progress(running.queue, uint32_t(running.value), "host1x wait");
}

void switch_gpu_producer_release(uint32_t syncpoint, uint32_t threshold)
{
	const int released = tegra_host1x_syncpt_release_through(syncpoint, threshold);
	if (released)
		IOLog("SwitchGpuBridge: syncpoint %u of a failed producer stays short of %u (%d)\n",
		      syncpoint, threshold, released);
}
