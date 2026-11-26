// SPDX-License-Identifier: BSD-2-Clause-Patent
// TegraHost1x.cpp's command DMA channels (SwitchHost1xChannels.cpp): each channel's push buffer of GATHERs and its
// command arena. `lock` is host1x's submit lock, which program and stop are called under.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <IOKit/IOLocks.h>

struct switch_dma_buffer;

bool switch_host1x_channels_alloc(volatile uint8_t *host, volatile uint8_t *sync, IOLock *lock);
// Every channel's push buffer and arena, the only memory host1x fetches: up to `room` of them into `out`.
uint32_t switch_host1x_channels_buffers(const switch_dma_buffer **out, uint32_t room);
// After alloc, before the first program: host1x's command DMA through the SMMU (SwitchHost1xSmmu.cpp), reaching those
// buffers and nothing else. False with one line in `line` when it could not be.
bool switch_host1x_channels_confine(char *line, size_t size);
// Boot and resume: every channel stopped, emptied and started again on its push buffer; submits are taken from here on.
// `clockKHz`: host1x's clock, which debug builds' channel counters count (163200 from PLLP / 2.5).
void switch_host1x_channels_program(uint32_t clockKHz);
// The channels (bit n: channel n) whose gather filter stayed on when program cleared it: their streams' host1x-class
// waits would not run.
uint32_t switch_host1x_channels_filter_stuck();
// Suspend: every channel stopped; submits fail until program.
void switch_host1x_channels_stop();

#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
// A fenced job takes syncpoint `id` to `threshold` (TegraHost1x.cpp's free check). Submit lock held.
void switch_host1x_syncpt_owed(uint32_t id, uint32_t threshold);
// A reset or teardown dropped a fenced job on `id`: no channel will take it further. Submit lock held.
void switch_host1x_syncpt_dropped(uint32_t id);
#endif

// Under the submit lock, host1x up, once a failed producer's cancellation is stored: the display stream that waits on
// it is cancelled, then the producer's syncpoint is released to `threshold`. An increment that does not take panics.
void switch_host1x_channels_release_failed(uint32_t id, uint32_t threshold);
