// SPDX-License-Identifier: MIT
// A bridge with no GPU job of its own (host1x.md A2): every pending token it is handed is a GM20B one on one channel,
// covered by that channel's own syncpoint increment (SwitchGpuTimelineSyncpt.cpp), so the consumer waits on that.
#pragma once
#include <stdint.h>
#include "SwitchGpuBacking.h"

struct SwitchGpuProducerWait {
	uint32_t syncpoint = 0;
	uint32_t threshold = 0;
	bool unassigned = false;  // `ask`'s channel has no syncpoint yet
	switch_gpu_backing_token ask = {};
};

// True when `wait` covers every token, so the bridge needs no GPU job. Takes the timelines' lock, not the bridge's.
bool switch_gpu_producer_wait(const switch_gpu_backing_token *tokens, uint32_t count,
			      SwitchGpuProducerWait &wait);
// Under the bridge's lock: gives the channel `wait` found without a syncpoint one, if one is free.
void switch_gpu_producer_assign(const SwitchGpuProducerWait &wait);
// The first of the tokens that is a GM20B job still running: what a waiter on a jobless bridge waits on.
bool switch_gpu_producer_running(const switch_gpu_backing_token *tokens, uint32_t count,
				 switch_gpu_backing_token &running);
// The hang rule's check of that job's channel (tgm_writer_check_progress); when the next check is due, 0 for none.
uint64_t switch_gpu_producer_check(const switch_gpu_backing_token &running);
// The producer failed, so its channel has stopped and the increments it owed never come: the CPU makes them, in thread
// context, after the jobs waiting on them were cancelled (tegra_host1x_syncpt_release_failed).
void switch_gpu_producer_release(uint32_t syncpoint, uint32_t threshold);
